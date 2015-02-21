//错误处理
#include "skynet.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_server.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LOG_MESSAGE_SIZE 256

void 
skynet_error(struct skynet_context * context, const char *msg, ...) {
	static uint32_t logger = 0;
	if (logger == 0) {
		logger = skynet_handle_findname("logger");//获取logger的句柄
	}
	if (logger == 0) {
		return;
	}

	char tmp[LOG_MESSAGE_SIZE];
	char *data = NULL;

// VA_LIST 是在C语言中解决变参问题的一组宏，在<stdarg.h>头文件下
// VA_LIST的用法：      
// （1）首先在函数里定义一个VA_LIST型的变量，这个变量是指向参数的指针
// （2）然后用VA_START宏初始化刚定义的VA_LIST变量，这个宏的第二个参数是第一个可变参数的前一个参数（也就是msg），是一个固定的参数。
// （3）然后用VA_ARG返回可变的参数，VA_ARG的第二个参数是你要返回的参数的类型。
// （4）最后用VA_END宏结束可变参数的获取。然后你就可以在函数里使用第二个参数了。如果函数有多个可变参数的，依次调用VA_ARG获取各个参数。
//  va:variable-argument
//  这里的用法是使用了vsnprintf将数据打印到字符串tmp中，数据格式由msg定义

	va_list ap;//定义一个VA_LIST型的变量

	va_start(ap,msg);//初始化刚定义的VA_LIST变量
	int len = vsnprintf(tmp, LOG_MESSAGE_SIZE, msg, ap);
	va_end(ap);
	if (len < LOG_MESSAGE_SIZE) {//写入的字符数没有超出
		data = skynet_strdup(tmp);//复制字符串
	} else {//写入的字符数超出了
		int max_size = LOG_MESSAGE_SIZE;
		for (;;) {//死循环
			max_size *= 2;//翻倍
			data = skynet_malloc(max_size);//分配内存

			//打印数据
			va_start(ap,msg);
			len = vsnprintf(data, max_size, msg, ap);
			va_end(ap);

			if (len < max_size) {//如果大小没有超出，则跳出循环，否则继续循环扩大缓冲区大小
				break;
			}
			skynet_free(data);//超出大小需要重新分配，所以释放刚分配的内存，防止内存泄漏
		}
	}


	struct skynet_message smsg;//skynet消息
	if (context == NULL) {//没有上下文
		smsg.source = 0;//来源为0
	} else {
		smsg.source = skynet_context_handle(context);//获取上下文的句柄
	}
	smsg.session = 0;//会话为0
	smsg.data = data;//数据为上面的data
	smsg.sz = len | (PTYPE_TEXT << HANDLE_REMOTE_SHIFT);//高8位为消息编码的协议类型
	skynet_context_push(logger, &smsg);//将消息push到上下文内的消息队列
}

