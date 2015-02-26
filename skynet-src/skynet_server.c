//Skynet 主要功能， 初始化组件、加载服务和通知服务
#include "skynet.h"

#include "skynet_server.h"
#include "skynet_module.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_timer.h"
#include "skynet_harbor.h"
#include "skynet_env.h"
#include "skynet_monitor.h"
#include "skynet_imp.h"
#include "skynet_log.h"

#include <pthread.h>

#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#ifdef CALLING_CHECK

//type __sync_lock_test_and_set (type *ptr, type value, ...)
//将*ptr设为value并返回*ptr操作之前的值

//void __sync_lock_release (type *ptr, ...)
//将*ptr置0

#define CHECKCALLING_BEGIN(ctx) assert(__sync_lock_test_and_set(&ctx->calling,1) == 0);	//检查调用开始
#define CHECKCALLING_END(ctx) __sync_lock_release(&ctx->calling);//检查调用结束
#define CHECKCALLING_INIT(ctx) ctx->calling = 0;//检查调用初始化
#define CHECKCALLING_DECL int calling;//检查调用声明

#else

#define CHECKCALLING_BEGIN(ctx)
#define CHECKCALLING_END(ctx)
#define CHECKCALLING_INIT(ctx)
#define CHECKCALLING_DECL

#endif
//skynet上下文数据结构定义
//上下文可以理解为一种环境，包含了许多东西
//因为skynet是基于actor模型的，所以这里的skynet_context其实也就是actor
//关于actor说明如下：
// Actor，可以看作是一个个独立的实体，他们之间是毫无关联的。但是，他们可以通过消息来通信。一个Actor收到其他Actor的信息后，它可以根据需要作出各种相应。消息的类型可以是任意的，消息的内容也可以是任意的。这点有点像webservice了。只提供接口服务，你不必了解我是如何实现的。
 
// 一个Actor如何处理多个Actor的请求呢？它先建立一个消息队列，每次收到消息后，就放入队列，而它每次也从队列中取出消息体来处理。通常我们都使得这个过程是循环的。让Actor可以时刻处理发送来的消息。

//但是在该blog(http://blog.codingnow.com/2013/12/skynet_monitor.html)中，云风说：“而且 skynet 借助 lua 的 coroutine 机制，事实上在同一个 lua service 里跑着多个 actor 。一个 lua coroutine 才是一个 actor ”

struct skynet_context {
	void * instance;			//模块实例引用
	struct skynet_module * mod;	//模块引用
	void * cb_ud;				//回调的用户数据(userdata)
	skynet_cb cb;				//skynet回调
	struct message_queue *queue;	//消息队列
	FILE * logfile;					//日志文件句柄
	char result[32];				//结果缓冲区
	uint32_t handle;				//句柄号
	int session_id;					//会话id，用于产生会话
	int ref;						//引用计数
	bool init;						//是否初始化
	bool endless;					//是否回绕

	CHECKCALLING_DECL				//检查调用声明
};
//skynet节点数据结构定义
struct skynet_node {
	int total;			//上下文计数器
	int init;
	uint32_t monitor_exit;//监视服务退出的句柄号
	pthread_key_t handle_key;
};

static struct skynet_node G_NODE;

int 
skynet_context_total() {
	return G_NODE.total;//获取全局的所有上下文数目
}

static void
context_inc() {//上下文计数增加
	__sync_fetch_and_add(&G_NODE.total,1);//原子操作	先fetch，然后自加，返回的是自加以前的值
}

static void
context_dec() {//上下文计数减少
	__sync_fetch_and_sub(&G_NODE.total,1);//原子操作
}

uint32_t 
skynet_current_handle(void) {
	if (G_NODE.init) {
		void * handle = pthread_getspecific(G_NODE.handle_key);
		return (uint32_t)(uintptr_t)handle;
	} else {
		uintptr_t v = (uint32_t)(-THREAD_MAIN);
		return v;
	}
}

static void
id_to_hex(char * str, uint32_t id) {
	int i;
	static char hex[16] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
	str[0] = ':';
	for (i=0;i<8;i++) {
		str[i+1] = hex[(id >> ((7-i) * 4))&0xf];
	}
	str[9] = '\0';
}

struct drop_t {
	uint32_t handle;
};

static void
drop_message(struct skynet_message *msg, void *ud) {
	struct drop_t *d = ud;
	skynet_free(msg->data);
	uint32_t source = d->handle;
	assert(source);
	// report error to the message source
	skynet_send(NULL, source, msg->source, PTYPE_ERROR, 0, NULL, 0);
}
//创建一个新的上下文
//参数为：
//name：模块名
//param：模块初始化时使用的参数
struct skynet_context * 
skynet_context_new(const char * name, const char *param) {
	struct skynet_module * mod = skynet_module_query(name);//根据名字查询模块

	if (mod == NULL)
		return NULL;

	void *inst = skynet_module_instance_create(mod);//创建模块实例
	if (inst == NULL)
		return NULL;
	struct skynet_context * ctx = skynet_malloc(sizeof(*ctx));//为skynet上下文分配内存
	CHECKCALLING_INIT(ctx)//检查调用初始化

	//初始化skynet上下文相关的一些数据
	ctx->mod = mod;//保存模块引用
	ctx->instance = inst;//保存模块实例引用
	ctx->ref = 2;//引用计数
	ctx->cb = NULL;
	ctx->cb_ud = NULL;
	ctx->session_id = 0;
	ctx->logfile = NULL;

	ctx->init = false;//标志是否已经初始化
	ctx->endless = false;//是否回绕？
	// Should set to 0 first to avoid skynet_handle_retireall get an uninitialized handle
	ctx->handle = 0;//初始化句柄号为0	

	ctx->handle = skynet_handle_register(ctx);//注册句柄
	struct message_queue * queue = ctx->queue = skynet_mq_create(ctx->handle);//创建消息队列
	// init function maybe use ctx->handle, so it must init at last
	context_inc();//全局上下文计数增加

	CHECKCALLING_BEGIN(ctx)
	int r = skynet_module_instance_init(mod, inst, ctx, param);//调用模块实例的初始化函数
	CHECKCALLING_END(ctx)
	if (r == 0) {//初始化成功
		struct skynet_context * ret = skynet_context_release(ctx);//因为上下文的引用计数初始化为2,所以这里调用一次release减1
		if (ret) {//如果返回值不为空
			ctx->init = true;//设置标志，标志完成初始化
		}
		skynet_globalmq_push(queue);//将队列push到全局队列中
		if (ret) {
			skynet_error(ret, "LAUNCH %s %s", name, param ? param : "");//打印日志
		}
		return ret;
	} else {
		skynet_error(ctx, "FAILED launch %s", name);//报错
		uint32_t handle = ctx->handle;//获取句柄
		skynet_context_release(ctx);//释放上下文
		skynet_handle_retire(handle);//回收句柄
		struct drop_t d = { handle };
		skynet_mq_release(queue, drop_message, &d);//释放队列
		return NULL;//返回空
	}
}

int
skynet_context_newsession(struct skynet_context *ctx) {
	// session always be a positive number
	int session = ++ctx->session_id;//自增会话ID产生一个会话
	if (session <= 0) {//如果会话小于0
		ctx->session_id = 1;//重置会话ID为1
		return 1;//返回会话1
	}
	return session;//返回产生的会话
}

void 
skynet_context_grab(struct skynet_context *ctx) {
	__sync_add_and_fetch(&ctx->ref,1);//上下文引用计数加1
}

void
skynet_context_reserve(struct skynet_context *ctx) {
	skynet_context_grab(ctx);
	// don't count the context reserved, because skynet abort (the worker threads terminate) only when the total context is 0 .
	// the reserved context will be release at last.
	context_dec();
}
//删除上下文
static void 
delete_context(struct skynet_context *ctx) {
	if (ctx->logfile) {//如果有日志文件句柄
		fclose(ctx->logfile);//关闭文件
	}
	skynet_module_instance_release(ctx->mod, ctx->instance);//释放模块实例
	skynet_mq_mark_release(ctx->queue);//释放队列
	skynet_free(ctx);//释放上下文
	context_dec();//全局上下文减少
}

struct skynet_context * 
skynet_context_release(struct skynet_context *ctx) {
	if (__sync_sub_and_fetch(&ctx->ref,1) == 0) {//引用计数为0
		delete_context(ctx);//删除上下文
		return NULL;//返回空
	}
	return ctx;//返回上下文
}

int
skynet_context_push(uint32_t handle, struct skynet_message *message) {
	struct skynet_context * ctx = skynet_handle_grab(handle);//根据句柄获取上下文引用
	if (ctx == NULL) {
		return -1;
	}
	skynet_mq_push(ctx->queue, message);//将消息放入队列
	skynet_context_release(ctx);//释放上下文，因为在skynet_handle_grab中调用了skynet_context_grab增加了上下文的引用计数

	return 0;
}

void 
skynet_context_endless(uint32_t handle) {
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return;
	}
	ctx->endless = true;
	skynet_context_release(ctx);
}

int 
skynet_isremote(struct skynet_context * ctx, uint32_t handle, int * harbor) {
	int ret = skynet_harbor_message_isremote(handle);
	if (harbor) {
		*harbor = (int)(handle >> HANDLE_REMOTE_SHIFT);
	}
	return ret;
}

static void
dispatch_message(struct skynet_context *ctx, struct skynet_message *msg) {
	assert(ctx->init);//断言此时上下文已经成功初始化
	CHECKCALLING_BEGIN(ctx)//检查调用开始
	pthread_setspecific(G_NODE.handle_key, (void *)(uintptr_t)(ctx->handle));//设置线程私有数据
	int type = msg->sz >> HANDLE_REMOTE_SHIFT;//获取消息类别
	size_t sz = msg->sz & HANDLE_MASK;//获取消息大小
	if (ctx->logfile) {//如果上下文内存在log文件
		skynet_log_output(ctx->logfile, msg->source, type, msg->session, msg->data, sz);//输出日志
	}
	if (!ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz)) {//调用服务的回到函数
		skynet_free(msg->data);//回调调用成功，释放消息承载的数据
	}
	CHECKCALLING_END(ctx)//检查调用结束
}

//分发上下文的所有消息
//说是分发，其实并没有发给别人，所有的消息已经在上下文内的队列里了
//只是等待时机从队列里把消息POP出来，调用上下文内服务的回调函数进行处理
void 
skynet_context_dispatchall(struct skynet_context * ctx) {
	// for skynet_error
	struct skynet_message msg;//定义一个skynet消息用于存储pop的消息
	struct message_queue *q = ctx->queue;//上下文内的队列
	while (!skynet_mq_pop(q,&msg)) {//一直从队列中pop消息，直到没有消息
		dispatch_message(ctx, &msg);//分发消息
	}
}

struct message_queue * 
skynet_context_message_dispatch(struct skynet_monitor *sm, struct message_queue *q, int weight) {
	if (q == NULL) {//如果传入的消息队列为空
		q = skynet_globalmq_pop();//从全局的队列中pop一个出来
		if (q==NULL)//如果消息队列还为空
			return NULL;//直接返回空
	}//如果传入的消息队列不为空，则是分发该消息队列的消息

	uint32_t handle = skynet_mq_handle(q);//从消息队列中取得句柄号

	struct skynet_context * ctx = skynet_handle_grab(handle);//从句柄号取得上下文
	if (ctx == NULL) {//如果取得的上下文为空
		struct drop_t d = { handle };
		skynet_mq_release(q, drop_message, &d);
		return skynet_globalmq_pop();
	}

	int i,n=1;
	struct skynet_message msg;//定义一个skynet消息用于存放收到的消息

	for (i=0;i<n;i++) {
		if (skynet_mq_pop(q,&msg)) {//
			skynet_context_release(ctx);
			return skynet_globalmq_pop();
		} else if (i==0 && weight >= 0) {
			n = skynet_mq_length(q);
			n >>= weight;
		}
		int overload = skynet_mq_overload(q);
		if (overload) {
			skynet_error(ctx, "May overload, message queue length = %d", overload);
		}

		skynet_monitor_trigger(sm, msg.source , handle);

		if (ctx->cb == NULL) {
			skynet_free(msg.data);
		} else {
			dispatch_message(ctx, &msg);
		}

		skynet_monitor_trigger(sm, 0,0);
	}

	assert(q == ctx->queue);
	struct message_queue *nq = skynet_globalmq_pop();
	if (nq) {
		// If global mq is not empty , push q back, and return next queue (nq)
		// Else (global mq is empty or block, don't push q back, and return q again (for next dispatch)
		skynet_globalmq_push(q);
		q = nq;
	} 
	skynet_context_release(ctx);

	return q;
}

static void
copy_name(char name[GLOBALNAME_LENGTH], const char * addr) {
	int i;
	for (i=0;i<GLOBALNAME_LENGTH && addr[i];i++) {
		name[i] = addr[i];
	}
	for (;i<GLOBALNAME_LENGTH;i++) {
		name[i] = '\0';
	}
}

uint32_t 
skynet_queryname(struct skynet_context * context, const char * name) {
	switch(name[0]) {
	case ':':
		return strtoul(name+1,NULL,16);
	case '.':
		return skynet_handle_findname(name + 1);
	}
	skynet_error(context, "Don't support query global name %s",name);
	return 0;
}

//处理退出
static void
handle_exit(struct skynet_context * context, uint32_t handle) {
	if (handle == 0) {//如果传入的句柄为0
		handle = context->handle;//设置句柄为当前上下文的句柄
		skynet_error(context, "KILL self");//输出日志 杀掉自己
	} else {
		skynet_error(context, "KILL :%0x", handle);//输出日志 杀掉句柄号为handle的服务
	}
	if (G_NODE.monitor_exit) {//如果存在监视服务退出的服务
		skynet_send(context,  handle, G_NODE.monitor_exit, PTYPE_CLIENT, 0, NULL, 0);//向该服务发送消息
	}
	skynet_handle_retire(handle);//回收句柄
}

// skynet command
//skynet命令数据结构定义
struct command_func {
	const char *name;	//命令名
	const char * (*func)(struct skynet_context * context, const char * param);	//命令函数
};

static const char *
cmd_timeout(struct skynet_context * context, const char * param) {
	char * session_ptr = NULL;
	int ti = strtol(param, &session_ptr, 10);
	int session = skynet_context_newsession(context);
	skynet_timeout(context->handle, ti, session);
	sprintf(context->result, "%d", session);
	return context->result;
}

static const char *
cmd_reg(struct skynet_context * context, const char * param) {
	if (param == NULL || param[0] == '\0') {//param为空或者为空字符串
		sprintf(context->result, ":%x", context->handle);//按十六进制将句柄号写入结果内
		return context->result;//返回结果
	} else if (param[0] == '.') {//param以字符'.'开头
		return skynet_handle_namehandle(context->handle, param + 1);//命名句柄
	} else {//其他情况非法
		skynet_error(context, "Can't register global name %s in C", param);
		//这里存在点问题，如果日志服务没有成功注册句柄名字，那在skynet_error里查找日志服务的句柄将找不到
		//那么该条错误信息就看不到了,不过该情况一般不会发生，除非你改了C的代码，因为日志服务注册的句柄名字为.logger
		return NULL;
	}
}

static const char *
cmd_query(struct skynet_context * context, const char * param) {
	if (param[0] == '.') {
		uint32_t handle = skynet_handle_findname(param+1);
		if (handle) {
			sprintf(context->result, ":%x", handle);
			return context->result;
		}
	}
	return NULL;
}

static const char *
cmd_name(struct skynet_context * context, const char * param) {
	int size = strlen(param);
	char name[size+1];
	char handle[size+1];
	sscanf(param,"%s %s",name,handle);
	if (handle[0] != ':') {
		return NULL;
	}
	uint32_t handle_id = strtoul(handle+1, NULL, 16);
	if (handle_id == 0) {
		return NULL;
	}
	if (name[0] == '.') {
		return skynet_handle_namehandle(handle_id, name + 1);
	} else {
		skynet_error(context, "Can't set global name %s in C", name);
	}
	return NULL;
}

static const char *
cmd_now(struct skynet_context * context, const char * param) {
	uint32_t ti = skynet_gettime();
	sprintf(context->result,"%u",ti);
	return context->result;
}
//退出命令
static const char *
cmd_exit(struct skynet_context * context, const char * param) {
	handle_exit(context, 0);
	return NULL;
}

static uint32_t
tohandle(struct skynet_context * context, const char * param) {
	uint32_t handle = 0;
	if (param[0] == ':') {
		handle = strtoul(param+1, NULL, 16);
	} else if (param[0] == '.') {
		handle = skynet_handle_findname(param+1);
	} else {
		skynet_error(context, "Can't convert %s to handle",param);
	}

	return handle;
}

static const char *
cmd_kill(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle) {
		handle_exit(context, handle);
	}
	return NULL;
}

static const char *
cmd_launch(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	char tmp[sz+1];
	strcpy(tmp,param);
	char * args = tmp;
	char * mod = strsep(&args, " \t\r\n");
	args = strsep(&args, "\r\n");
	struct skynet_context * inst = skynet_context_new(mod,args);
	if (inst == NULL) {
		return NULL;
	} else {
		id_to_hex(context->result, inst->handle);
		return context->result;
	}
}

static const char *
cmd_getenv(struct skynet_context * context, const char * param) {
	return skynet_getenv(param);
}

static const char *
cmd_setenv(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	char key[sz+1];
	int i;
	for (i=0;param[i] != ' ' && param[i];i++) {
		key[i] = param[i];
	}
	if (param[i] == '\0')
		return NULL;

	key[i] = '\0';
	param += i+1;
	
	skynet_setenv(key,param);
	return NULL;
}

static const char *
cmd_starttime(struct skynet_context * context, const char * param) {
	uint32_t sec = skynet_gettime_fixsec();
	sprintf(context->result,"%u",sec);
	return context->result;
}

static const char *
cmd_endless(struct skynet_context * context, const char * param) {
	if (context->endless) {
		strcpy(context->result, "1");
		context->endless = false;
		return context->result;
	}
	return NULL;
}

static const char *
cmd_abort(struct skynet_context * context, const char * param) {
	skynet_handle_retireall();
	return NULL;
}

static const char *
cmd_monitor(struct skynet_context * context, const char * param) {
	uint32_t handle=0;
	if (param == NULL || param[0] == '\0') {
		if (G_NODE.monitor_exit) {
			// return current monitor serivce
			sprintf(context->result, ":%x", G_NODE.monitor_exit);
			return context->result;
		}
		return NULL;
	} else {
		handle = tohandle(context, param);
	}
	G_NODE.monitor_exit = handle;
	return NULL;
}

static const char *
cmd_mqlen(struct skynet_context * context, const char * param) {
	int len = skynet_mq_length(context->queue);
	sprintf(context->result, "%d", len);
	return context->result;
}

static const char *
cmd_logon(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	FILE *f = NULL;
	FILE * lastf = ctx->logfile;
	if (lastf == NULL) {
		f = skynet_log_open(context, handle);
		if (f) {
			if (!__sync_bool_compare_and_swap(&ctx->logfile, NULL, f)) {
				// logfile opens in other thread, close this one.
				fclose(f);
			}
		}
	}
	skynet_context_release(ctx);
	return NULL;
}

static const char *
cmd_logoff(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	FILE * f = ctx->logfile;
	if (f) {
		// logfile may close in other thread
		if (__sync_bool_compare_and_swap(&ctx->logfile, f, NULL)) {
			skynet_log_close(context, f, handle);
		}
	}
	skynet_context_release(ctx);
	return NULL;
}
//命令名-》命令函数 映射表
static struct command_func cmd_funcs[] = {
	{ "TIMEOUT", cmd_timeout },
	{ "REG", cmd_reg },//注册命令
	{ "QUERY", cmd_query },
	{ "NAME", cmd_name },
	{ "NOW", cmd_now },
	{ "EXIT", cmd_exit },//退出命令
	{ "KILL", cmd_kill },
	{ "LAUNCH", cmd_launch },
	{ "GETENV", cmd_getenv },
	{ "SETENV", cmd_setenv },
	{ "STARTTIME", cmd_starttime },
	{ "ENDLESS", cmd_endless },
	{ "ABORT", cmd_abort },
	{ "MONITOR", cmd_monitor },
	{ "MQLEN", cmd_mqlen },
	{ "LOGON", cmd_logon },
	{ "LOGOFF", cmd_logoff },
	{ NULL, NULL },
};
//从命令表中找到一个命令执行
const char * 
skynet_command(struct skynet_context * context, const char * cmd , const char * param) {
	struct command_func * method = &cmd_funcs[0];//初始化指针，指向映射表的第一项
	while(method->name) {//如果该项名字不为空
		if (strcmp(cmd, method->name) == 0) {//比较名字是否匹配
			return method->func(context, param);//调用命令函数
		}
		++method;//指向到下一项
	}

	return NULL;
}
//过滤参数
static void
_filter_args(struct skynet_context * context, int type, int *session, void ** data, size_t * sz) {
	int needcopy = !(type & PTYPE_TAG_DONTCOPY);//判断是否需要复制数据
	int allocsession = type & PTYPE_TAG_ALLOCSESSION;//判断是否需要分配会话
	type &= 0xff;//type的低8位表示了消息的类型

	if (allocsession) {//如果需要分配会话
		assert(*session == 0);//断言传入的session为0
		*session = skynet_context_newsession(context);//分配一个会话
	}

	if (needcopy && *data) {//如果需要复制数据并且指向数据的指针不为空
		char * msg = skynet_malloc(*sz+1);//分配内存
		memcpy(msg, *data, *sz);//内存复制
		msg[*sz] = '\0';//添加一个字符串结束符
		*data = msg;//让指向数据的指针指向新的内存地址
	}

	*sz |= type << HANDLE_REMOTE_SHIFT;//设置消息的大小
}
//skynet发送消息
int
skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * data, size_t sz) {
	if ((sz & HANDLE_MASK) != sz) {//判断消息是否过大
		skynet_error(context, "The message to %x is too large (sz = %lu)", destination, sz);
		skynet_free(data);
		return -1;
	}
	_filter_args(context, type, &session, (void **)&data, &sz);//过滤参数

	if (source == 0) {//如果源地址为0
		source = context->handle;//设置源地址为上下文的句柄，那么源就是自己
	}

	if (destination == 0) {//如果目的地址为0
		return session;//则返回会话
	}
	if (skynet_harbor_message_isremote(destination)) {//判断消息是否是远程消息
		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		rmsg->destination.handle = destination;
		rmsg->message = data;
		rmsg->sz = sz;
		skynet_harbor_send(rmsg, source, session);//用harbor将消息发送出去
	} else {//消息为本地消息
		struct skynet_message smsg;//定义一个skynet消息
		smsg.source = source;//设置源地址
		smsg.session = session;//设置会话
		smsg.data = data;//设置数据
		smsg.sz = sz;//设置数据大小

		if (skynet_context_push(destination, &smsg)) {//发送消息到目标的队列中
			skynet_free(data);//push消息到队列失败，则释放数据
			return -1;
		}
	}
	return session;//返回会话
}

int
skynet_sendname(struct skynet_context * context, uint32_t source, const char * addr , int type, int session, void * data, size_t sz) {
	if (source == 0) {
		source = context->handle;
	}
	uint32_t des = 0;
	if (addr[0] == ':') {
		des = strtoul(addr+1, NULL, 16);
	} else if (addr[0] == '.') {
		des = skynet_handle_findname(addr + 1);
		if (des == 0) {
			if (type & PTYPE_TAG_DONTCOPY) {
				skynet_free(data);
			}
			return -1;
		}
	} else {
		_filter_args(context, type, &session, (void **)&data, &sz);

		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		copy_name(rmsg->destination.name, addr);
		rmsg->destination.handle = 0;
		rmsg->message = data;
		rmsg->sz = sz;

		skynet_harbor_send(rmsg, source, session);
		return session;
	}

	return skynet_send(context, source, des, type, session, data, sz);
}

//获取上下文的句柄
uint32_t 
skynet_context_handle(struct skynet_context *ctx) {
	return ctx->handle;
}

void 
skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb) {
	context->cb = cb;
	context->cb_ud = ud;
}

void
skynet_context_send(struct skynet_context * ctx, void * msg, size_t sz, uint32_t source, int type, int session) {
	struct skynet_message smsg;
	smsg.source = source;
	smsg.session = session;
	smsg.data = msg;
	smsg.sz = sz | type << HANDLE_REMOTE_SHIFT;

	skynet_mq_push(ctx->queue, &smsg);
}

void
skynet_globalinit(void) {
	//初始化全局节点G_NODE
	G_NODE.total = 0;
	G_NODE.monitor_exit = 0;
	G_NODE.init = 1;
	//创建线程私有数据
	if (pthread_key_create(&G_NODE.handle_key, NULL)) {
		fprintf(stderr, "pthread_key_create failed");
		exit(1);
	}
	// set mainthread's key 
	//设置主线程的key
	skynet_initthread(THREAD_MAIN);
}

void 
skynet_globalexit(void) {
	//注销线程私有数据
	pthread_key_delete(G_NODE.handle_key);
}

void
skynet_initthread(int m) {
	uintptr_t v = (uint32_t)(-m);//为了和service address显出不同来，这个就是做 log 用的
	pthread_setspecific(G_NODE.handle_key, (void *)v);//设置线程私有数据
}

