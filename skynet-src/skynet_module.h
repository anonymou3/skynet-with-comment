//动态链接库(模块)加载
#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

struct skynet_context;

typedef void * (*skynet_dl_create)(void);
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);
typedef void (*skynet_dl_release)(void * inst);

//skynet模块数据结构定义
struct skynet_module {
	const char * name;//模块名字
	void * module;//模块句柄，此句柄不是skynet中handle
	skynet_dl_create create;//创建函数
	skynet_dl_init init;//初始化函数
	skynet_dl_release release;//释放函数
};

void skynet_module_insert(struct skynet_module *mod);
struct skynet_module * skynet_module_query(const char * name);
void * skynet_module_instance_create(struct skynet_module *);
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);
void skynet_module_instance_release(struct skynet_module *, void *inst);

void skynet_module_init(const char *path);

#endif
