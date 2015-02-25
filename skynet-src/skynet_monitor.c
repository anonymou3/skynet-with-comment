//监视服务
#include "skynet.h"

#include "skynet_monitor.h"
#include "skynet_server.h"
#include "skynet.h"

#include <stdlib.h>
#include <string.h>

//监视者数据结构定义
struct skynet_monitor {
	int version;		//版本
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
//
void 
skynet_monitor_trigger(struct skynet_monitor *sm, uint32_t source, uint32_t destination) {
	sm->source = source;
	sm->destination = destination;
	__sync_fetch_and_add(&sm->version , 1);
}

void 
skynet_monitor_check(struct skynet_monitor *sm) {
	if (sm->version == sm->check_version) {//
		if (sm->destination) {
			skynet_context_endless(sm->destination);
			skynet_error(NULL, "A message from [ :%08x ] to [ :%08x ] maybe in an endless loop (version = %d)", sm->source , sm->destination, sm->version);
		}
	} else {
		sm->check_version = sm->version;//
	}
}
