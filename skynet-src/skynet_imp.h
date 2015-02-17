//启动流程
#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H

//skynet 配置数据结构
struct skynet_config {
	int thread;					//工作线程数
	int harbor;
	const char * daemon;
	const char * module_path;
	const char * bootstrap;
	const char * logger;
};

#define THREAD_WORKER 0		//工作线程
#define THREAD_MAIN 1		//主线程
#define THREAD_SOCKET 2		//socket线程
#define THREAD_TIMER 3 		//定时器线程
#define THREAD_MONITOR 4 	//监控线程

void skynet_start(struct skynet_config * config);

#endif
