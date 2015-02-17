//动态链接库(模块)加载
#include "skynet.h"

#include "skynet_module.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_MODULE_TYPE 32

//模块集合数据结构定义
struct modules {
	int count;//模块计数器
	int lock;//锁
	const char * path;//模块所在路径
	struct skynet_module m[MAX_MODULE_TYPE];//存储具体的各个模块
};

static struct modules * M = NULL;

static void *
_try_open(struct modules *m, const char * name) {
	const char *l;
	const char * path = m->path;//模块所在路径
	size_t path_size = strlen(path);//模块路径字符串长度
	size_t name_size = strlen(name);//模块名字字符串名字

	int sz = path_size + name_size;//模块路径+名字字符串长度
	//search path
	void * dl = NULL;//库引用
	char tmp[sz];
	do
	{
		memset(tmp,0,sz);//清空内存
		while (*path == ';') path++;//跳过字符';'
		if (*path == '\0') break;//已经到达了字符串结尾 跳出循环
		l = strchr(path, ';');//查找字符串中首次出现字符';'的位置
		if (l == NULL) l = path + strlen(path);//字符串中没有字符';' 将l指向字符串的末尾
		int len = l - path;//计算路径长度
		int i;
		for (i=0;path[i]!='?' && i < len ;i++) {
			tmp[i] = path[i];//循环取出路径放入tmp
		}
		memcpy(tmp+i,name,name_size);//在tmp后面追加名字，组成完整的路径（路径+名字）
		if (path[i] == '?') {//检查配置中的通配符是否为？
			strncpy(tmp+i+name_size,path+i+1,len - i - 1);//追加后缀名
		} else {//配置中的通配符是非法的
			fprintf(stderr,"Invalid C service path\n");//非法的C服务路径
			exit(1);
		}
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);//打开动态库，返回动态库句柄
		path = l;//将path指向下一个路径
	}while(dl == NULL);

	if (dl == NULL) {//尝试打开动态库失败
		fprintf(stderr, "try open %s failed : %s\n",name,dlerror());
	}
 
	return dl;//返回动态库句柄
}

static struct skynet_module * 
_query(const char * name) {
	int i;
	for (i=0;i<M->count;i++) {//遍历已有模块
		if (strcmp(M->m[i].name,name)==0) {//比较名字是否匹配
			return &M->m[i];//返回模块
		}
	}
	return NULL;
}

static int
_open_sym(struct skynet_module *mod) {
	size_t name_size = strlen(mod->name);//名字大小
	char tmp[name_size + 9]; // create/init/release , longest name is release (7)分配足够的缓冲区
	memcpy(tmp, mod->name, name_size);//拷贝模块名字
	strcpy(tmp+name_size, "_create");//追加函数名 create函数
	mod->create = dlsym(mod->module, tmp);//根据动态链接库操作句柄与符号，返回符号对应的地址
	strcpy(tmp+name_size, "_init");//init函数
	mod->init = dlsym(mod->module, tmp);//获取符号对应的地址
	strcpy(tmp+name_size, "_release");//release函数
	mod->release = dlsym(mod->module, tmp);//获取符号对应的地址

	return mod->init == NULL;//mod->init不为NULL，则返回0，代表打开符号成功
}

struct skynet_module * 
skynet_module_query(const char * name) {
	struct skynet_module * result = _query(name);//根据名字查询模块
	if (result)
		return result;
	
	while(__sync_lock_test_and_set(&M->lock,1)) {}//加锁

	result = _query(name); // double check	再次确认

	if (result == NULL && M->count < MAX_MODULE_TYPE) {//没有找到对应的模块并且模块集合还有位置存储模块
		int index = M->count;//取得模块集合的索引
		void * dl = _try_open(M,name);//打开动态库
		if (dl) {//打开动态库成功
			M->m[index].name = name;//保存名字
			M->m[index].module = dl;//保存句柄

			if (_open_sym(&M->m[index]) == 0) {//打开符号成功
				M->m[index].name = skynet_strdup(name);//为什么要重新分配内存存储字符串？
				M->count ++;//增加计数器
				result = &M->m[index];//得到存储模块的引用
			}
		}
	}

	__sync_lock_release(&M->lock);//解锁

	return result;//返回模块引用
}

void 
skynet_module_insert(struct skynet_module *mod) {
	while(__sync_lock_test_and_set(&M->lock,1)) {}

	struct skynet_module * m = _query(mod->name);
	assert(m == NULL && M->count < MAX_MODULE_TYPE);
	int index = M->count;
	M->m[index] = *mod;
	++M->count;
	__sync_lock_release(&M->lock);
}

void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {//如果存在create函数
		return m->create();//调用create函数
	} else {
		return (void *)(intptr_t)(~0);
	}
}

int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

void 
skynet_module_init(const char *path) {
	struct modules *m = skynet_malloc(sizeof(*m));//分配内存
	//赋值
	m->count = 0;
	m->path = skynet_strdup(path);
	m->lock = 0;
	//保存指针
	M = m;
}
