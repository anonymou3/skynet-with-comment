//启动流程
#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//监视者们数据结构定义
struct monitor {
	int count;//监视者计数
	struct skynet_monitor ** m;//存储具体的监视者
	pthread_cond_t cond;//条件锁	
	pthread_mutex_t mutex;//互斥锁
	//为了防止竞争，条件锁的使用总是和一个互斥锁结合在一起
	//条件锁是用来线程通讯的，但是互斥锁是为了保护这种通讯不会产生逻辑错误，可以正常工作。
	int sleep;//睡眠
};

//工作者参数数据结构定义
struct worker_parm {
	struct monitor *m;//监视者们引用
	int id;			//ID
	int weight;		//权重
};

#define CHECK_ABORT if (skynet_context_total()==0) break;//检查是否停止，根据上下文的数目

//创建线程包装函数
static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

//唤醒函数
static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// signal sleep worker, "spurious wakeup" is harmless
		pthread_cond_signal(&m->cond);
	}
}

//socket工作函数
static void *
_socket(void *p) {
	struct monitor * m = p;//获取到监视者们
	skynet_initthread(THREAD_SOCKET);//初始化线程私有数据
	for (;;) {//死循环
		int r = skynet_socket_poll();
		if (r==0)
			break;
		if (r<0) {
			CHECK_ABORT
			continue;
		}
		wakeup(m,0);
	}
	return NULL;
}

static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	skynet_free(m->m);
	skynet_free(m);
}

//监视线程工作函数
static void *
_monitor(void *p) {
	struct monitor * m = p;//先从p中获得监视者们引用
	int i;
	int n = m->count;//获取监视者数目
	skynet_initthread(THREAD_MONITOR);//初始化线程私有数据
	for (;;) {//死循环
		CHECK_ABORT//检查是否跳出死循环
		for (i=0;i<n;i++) {//遍历具体的监视者
			skynet_monitor_check(m->m[i]);//具体的监视者检查
		}
		for (i=0;i<5;i++) {//循环5次
			CHECK_ABORT//检查是否跳出死循环
			sleep(1);//本线程休眠1毫秒，放弃CPU，让其他线程得到执行
		}
	}

	return NULL;
}

//时钟线程工作函数
static void *
_timer(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_TIMER);
	for (;;) {
		skynet_updatetime();
		CHECK_ABORT
		wakeup(m,m->count-1);
		usleep(2500);
	}
	// wakeup socket thread
	skynet_socket_exit();
	// wakeup all worker thread
	pthread_cond_broadcast(&m->cond);
	return NULL;
}

//工作线程工作函数
static void *
_worker(void *p) {
	struct worker_parm *wp = p;//获取工作线程参数引用
	int id = wp->id;//获取id
	int weight = wp->weight;//获取权重
	struct monitor *m = wp->m;//获取到监视者们
	struct skynet_monitor *sm = m->m[id];//根据id从监视者们获取到具体的监视者引用
	skynet_initthread(THREAD_WORKER);//线程私有数据初始化
	struct message_queue * q = NULL;//定义消息队列指针
	for (;;) {//死循环
		q = skynet_context_message_dispatch(sm, q, weight);
		if (q == NULL) {//如果返回的消息队列为空
			if (pthread_mutex_lock(&m->mutex) == 0) {
				++ m->sleep;
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
				pthread_cond_wait(&m->cond, &m->mutex);
				-- m->sleep;
				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		} 
		CHECK_ABORT//检查是否跳出死循环
	}
	return NULL;
}

static void
_start(int thread) {
	pthread_t pid[thread+3];//保存线程ID的数组，多分配了3个槽

	struct monitor *m = skynet_malloc(sizeof(*m));//分配monitor内存
	memset(m, 0, sizeof(*m));//清空monitor内存
	m->count = thread;//监视者们的数目同线程数相同
	m->sleep = 0;//睡眠？

	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));//为存储具体的监视者分配内存
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new();//创建监视者并存储
	}
	if (pthread_mutex_init(&m->mutex, NULL)) {//初始化互斥锁
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	if (pthread_cond_init(&m->cond, NULL)) {//初始化条件锁
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	create_thread(&pid[0], _monitor, m);//创建监视线程
	create_thread(&pid[1], _timer, m);//创建时钟线程
	create_thread(&pid[2], _socket, m);//创建socket线程

	static int weight[] = {//权重
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 
		2, 2, 2, 2, 2, 2, 2, 2, 
		3, 3, 3, 3, 3, 3, 3, 3, };
	struct worker_parm wp[thread];
	for (i=0;i<thread;i++) {
		wp[i].m = m;//设置监视者们引用
		wp[i].id = i;//设置ID
		//sizeof(weight)/sizeof(weight[0])是计算出权重数组的大小，现在大小为24
		if (i < sizeof(weight)/sizeof(weight[0])) {//当i小于权重数组的时候
			wp[i].weight= weight[i];//权重值为数组内的值
		} else {
			wp[i].weight = 0;//当大于等于权重数组的时候，权重值为0
		}
		create_thread(&pid[i+3], _worker, &wp[i]);//创建工作线程
	}

	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL);//等待线程们退出 
	}

	free_monitor(m);//释放监视者们和具体的监视者
}

static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	int sz = strlen(cmdline);//计算参数的长度	默认的 bootstrap 配置项为 "snlua bootstrap"
	char name[sz+1];//存储“snlua”
	char args[sz+1];//存储"bootstrap"
	sscanf(cmdline, "%s %s", name, args);//拆解参数
	struct skynet_context *ctx = skynet_context_new(name, args)//新建上下文(这里为snlua)
	if (ctx == NULL) {
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		skynet_context_dispatchall(logger);//分发logger服务的所有消息
		exit(1);
	}
}

void 
skynet_start(struct skynet_config * config) {
	if (config->daemon) {//如果以守护进程启动
		if (daemon_init(config->daemon)) {//初始化
			exit(1);
		}
	}
	//初始化各个组件
	skynet_harbor_init(config->harbor);//harbor初始化
	skynet_handle_init(config->harbor);//句柄初始化
	skynet_mq_init();//消息队列初始化
	skynet_module_init(config->module_path);//模块初始化
	skynet_timer_init();//时钟初始化
	skynet_socket_init();//socket初始化

	struct skynet_context *ctx = skynet_context_new("logger", config->logger);//加载日志模块
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch logger service\n");
		exit(1);
	}

	bootstrap(ctx, config->bootstrap);//加载引导模块,传入的ctx是日志模块的上下文

	_start(config->thread);//启动各个线程

	// harbor_exit may call socket send, so it should exit before socket_free
	// harbor_exit 可能会调用 socket send,所以他应该在socket_free之前退出
	skynet_harbor_exit();//harbor退出
	skynet_socket_free();//释放socket
	if (config->daemon) {//如果以守护进程启动
		daemon_exit(config->daemon);//守护进程退出
	}
}
