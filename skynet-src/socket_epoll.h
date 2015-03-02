//网络模块
#ifndef poll_socket_epoll_h
#define poll_socket_epoll_h

#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

//efd:epoll fd
//判断efd是否合法
static bool 
sp_invalid(int efd) {
	return efd == -1;//如果efd为-1,则为非法
}

static int
sp_create() {//创建socket poll
	// epoll_create() 返回一个引用创建 epoll 实例的文件描述符
	// 这个文件描述符用于接下来的所有 epoll 的接口
	return epoll_create(1024);
	// epoll_create() 创建一个 epoll 实例，并要求内核分配一个可以保存 size 个描述符的空间。size 不是可以保存的最大的描述符个数，而只是给内核规划内部结构的一个暗示。
	// 从 Linux 2.6.8 以后，参数 size 不再使用，但必需大于零。(内核动态地分配数据结构而不管这个初始的暗示。)
}

static void//释放socket poll
sp_release(int efd) {
	//当没有更多请求时，epoll_create() 返回的文件描述符应该由 close 关闭
	//当所有引用一个 epoll 实例的文件描述符都关闭时，内核将销毁该实例并释放相关资源以重新利用。
	close(efd);//调用close关闭
}

static int 
sp_add(int efd, int sock, void *ud) {
	struct epoll_event ev;
	ev.events = EPOLLIN;//相关联的文件对 read 操作有效	默认开启读，没有开启写
	ev.data.ptr = ud; //用户数据


	//把目标文件描述符 sock 注册到由 efd 引用的 epoll 实例上并把相应的事件 ev 与内部的 sock 相链接
	if (epoll_ctl(efd, EPOLL_CTL_ADD, sock, &ev) == -1) {
		return 1;//epoll_ctl出错
	}
	return 0;//epoll_ctl成功
}

static void 
sp_del(int efd, int sock) {
	//从由 efd 引用的 epoll 实例中删除目标文件描述符 sock
	epoll_ctl(efd, EPOLL_CTL_DEL, sock , NULL);
}

//开启写
//enable为true则开启，为false关闭
static void 
sp_write(int efd, int sock, void *ud, bool enable) {
	struct epoll_event ev;
	ev.events = EPOLLIN | (enable ? EPOLLOUT : 0);//设置事件
	ev.data.ptr = ud;//用户数据
	//更改目标文件描述符 sock 相关联的事件 event
	epoll_ctl(efd, EPOLL_CTL_MOD, sock, &ev);
}

static int 
sp_wait(int efd, struct event *e, int max) {
	struct epoll_event ev[max];//定义一个事件数组
	int n = epoll_wait(efd , ev, max, -1);//等待事件
	int i;
	for (i=0;i<n;i++) {
		e[i].s = ev[i].data.ptr;//将用户数据返回
		unsigned flag = ev[i].events;//取出事件
		e[i].write = (flag & EPOLLOUT) != 0;//获取是否可写
		e[i].read = (flag & EPOLLIN) != 0;//获取是否可读
	}

	return n;//返回就绪的请求的 I/O 个数
}

//改变文件描述符fd IO为非阻塞
static void
sp_nonblocking(int fd) {
	int flag = fcntl(fd, F_GETFL, 0);//取得文件描述符状态flag，此flag为open（）的参数flags
	if ( -1 == flag ) {//fcntl出错
		return;
	}

	fcntl(fd, F_SETFL, flag | O_NONBLOCK);//设置状态flag	使I/O变成非阻塞
}

#endif
