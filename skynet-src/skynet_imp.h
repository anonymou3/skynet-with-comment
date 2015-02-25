//启动流程
#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H

//skynet 配置数据结构
struct skynet_config {
	int thread;					//工作线程数
	int harbor;					//节点号
	const char * daemon;		//守护进程配置
	const char * module_path;	//模块目录配置
	const char * bootstrap;		//引导配置
	const char * logger;		//日志配置
};

#define THREAD_WORKER 0		//工作线程
#define THREAD_MAIN 1		//主线程
#define THREAD_SOCKET 2		//socket线程
#define THREAD_TIMER 3 		//时钟线程
#define THREAD_MONITOR 4 	//监控线程

void skynet_start(struct skynet_config * config);

#endif
