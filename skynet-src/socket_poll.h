//网络接口层定义,也可以叫做封装层
#ifndef socket_poll_h
#define socket_poll_h

#include <stdbool.h>

//epoll kqueue统称poll

typedef int poll_fd;//poll文件描述符

//事件数据结构定义(封装epoll kqueue)
struct event {
	void * s;//用户数据
	bool read;//是否可读
	bool write;//是否可写
};

//sp:socket poll
//接口定义
static bool sp_invalid(poll_fd fd);
static poll_fd sp_create();
static void sp_release(poll_fd fd);
static int sp_add(poll_fd fd, int sock, void *ud);
static void sp_del(poll_fd fd, int sock);
static void sp_write(poll_fd, int sock, void *ud, bool enable);
static int sp_wait(poll_fd, struct event *e, int max);
static void sp_nonblocking(int sock);

//实现在下面，根据平台的不同包含不同的实现代码
#ifdef __linux__	//如果是linux平台
#include "socket_epoll.h"	//使用epoll
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined (__NetBSD__)	//如果是apple freebsd openbsd netbsd
#include "socket_kqueue.h"	//使用kqueue
#endif

#endif
