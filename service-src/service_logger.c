//日志服务
//模块规则：
//模块包含create函数，init函数，release函数
//函数名为：模块名_函数名，例如：create函数为logger_create
#include "skynet.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

//logger数据结构
struct logger {
	FILE * handle;//日志文件指针
	int close;//release时是否需要调用fclose
};

struct logger *
logger_create(void) {
	struct logger * inst = skynet_malloc(sizeof(*inst));
	inst->handle = NULL;
	inst->close = 0;
	return inst;
}

void
logger_release(struct logger * inst) {
	if (inst->close) {
		fclose(inst->handle);
	}
	skynet_free(inst);
}

static int
_logger(struct skynet_context * context, void *ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct logger * inst = ud;
	fprintf(inst->handle, "[:%08x] ",source);
	fwrite(msg, sz , 1, inst->handle);
	fprintf(inst->handle, "\n");
	fflush(inst->handle);

	return 0;
}

int
logger_init(struct logger * inst, struct skynet_context *ctx, const char * parm) {
	if (parm) {//如果param不为空，则将日志输出到文件
		inst->handle = fopen(parm,"w");//以可写模式打开文件
		if (inst->handle == NULL) {//打开文件失败
			return 1;//失败
		}
		inst->close = 1;//release时需要调用fclose
	} else {//如果param为空，则将日志输出到标准输出
		inst->handle = stdout;
	}
	if (inst->handle) {//或者输出到文件，或者输出到标准输出
		skynet_callback(ctx, inst, _logger);//设置回调
		skynet_command(ctx, "REG", ".logger");//注册日志全局名字
		return 0;//成功
	}
	return 1;//失败
}
