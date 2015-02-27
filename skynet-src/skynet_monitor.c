//监视服务
//作用：处理 service 生命期监控
#include "skynet.h"

#include "skynet_monitor.h"
#include "skynet_server.h"
#include "skynet.h"

#include <stdlib.h>
#include <string.h>


//监视者数据结构定义
struct skynet_monitor {
	int version;		//当前版本
	int check_version;	//检查版本
	uint32_t source;	//源
	uint32_t destination;	//目标
};

//新建监视者
struct skynet_monitor * 
skynet_monitor_new() {
	struct skynet_monitor * ret = skynet_malloc(sizeof(*ret));//为具体的监视者分配内存
	memset(ret, 0, sizeof(*ret));//清理内存
	return ret;//返回具体的监视者引用
}
//删除监视者
void 
skynet_monitor_delete(struct skynet_monitor *sm) {
	skynet_free(sm);//删除监视者直接释放掉其占用的内存即可
}

//触发监视器
//在派发消息之前触发监视器(设置sm->source sm->destination)
//在派发消息之后重置监视器(同样是调用skynet_monitor_trigger)，为什么调用同一个函数，效果不同？因为在sm->destination为0的时候，检查监视器是毫无效果的
//而在派发消息之前触发了监视器，但是派发消息时陷入死循环，那么就没有重置监视器，在检查监视器的时候，就会出现
//sm->version == sm->check_version并且sm->destination不为0了，就要报告错误了(skynet_error)
void 
skynet_monitor_trigger(struct skynet_monitor *sm, uint32_t source, uint32_t destination) {
	sm->source = source;//设置源
	sm->destination = destination;//设置目的
	__sync_fetch_and_add(&sm->version , 1);//自增版本
}

//检查监视器
void 
skynet_monitor_check(struct skynet_monitor *sm) {
	if (sm->version == sm->check_version) {//如果当前版本等于检查版本
		if (sm->destination) {//如果目的不为0
			skynet_context_endless(sm->destination);//设置目的上下文的endless标志
			skynet_error(NULL, "A message from [ :%08x ] to [ :%08x ] maybe in an endless loop (version = %d)", sm->source , sm->destination, sm->version);//一个从source到destination的消息可能陷入死循环
		}
	} else {
		sm->check_version = sm->version;//设置检查版本等于当前版本
	}
}
