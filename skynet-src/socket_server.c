//网络模块
#include "skynet.h"

#include "socket_server.h"
#include "socket_poll.h"//提供统一接口，根据不同的平台使用epoll或kqueue

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#define MAX_INFO 128

// MAX_SOCKET will be 2^MAX_SOCKET_P
//连接总数不超过2^16=65536
#define MAX_SOCKET_P 16　　　　　　　//最大socket数的指数

#define MAX_EVENT 64　　　　　　　　//最大事件数　　　
#define MIN_READ_BUFFER 64　　　　//最小读缓冲大小
#define SOCKET_TYPE_INVALID 0 	//无效的socket
#define SOCKET_TYPE_RESERVE 1 	//预留已经被申请 即将被使用
#define SOCKET_TYPE_PLISTEN 2 	//listen fd但是未加入epoll管理（加入epoll管理：调用sp_add）
#define SOCKET_TYPE_LISTEN 3 	//监听到套接字已经加入epoll管理
#define SOCKET_TYPE_CONNECTING 4 	//尝试连接的socket fd
#define SOCKET_TYPE_CONNECTED 5 	//已经建立连接的socket 主动conn（SOCKET_TYPE_CONNECTING）或者被动accept（SOCKET_TYPE_PACCEPT）的套接字 已经加入epoll管理
#define SOCKET_TYPE_HALFCLOSE 6 	//已经在应用层关闭了fd 但是数据还没有发送完 还没有close
#define SOCKET_TYPE_PACCEPT 7 		//accept返回的fd 未加入epoll
#define SOCKET_TYPE_BIND 8 			//其他类型的fd 如 stdin stdout等

#define MAX_SOCKET (1<<MAX_SOCKET_P)　//最大socket数

#define PRIORITY_HIGH 0	//高优先级
#define PRIORITY_LOW 1　//低优先级

#define HASH_ID(id) (((unsigned)id) % MAX_SOCKET)

#define PROTOCOL_TCP 0			//TCP
#define PROTOCOL_UDP 1			//UDP
#define PROTOCOL_UDPv6 2		//UDP ipv6

#define UDP_ADDRESS_SIZE 19	// ipv6 128bit + port 16bit + 1 byte type  16+2+1=19

#define MAX_UDP_PACKAGE 65535

//写缓冲数据结构定义
struct write_buffer {
	struct write_buffer * next; // 发送缓冲区构成一个链表
	void *buffer;
	char *ptr;	// 指向当前未发送的数据首部
	int sz;
	bool userobject;
	uint8_t udp_address[UDP_ADDRESS_SIZE];
};

#define SIZEOF_TCPBUFFER (offsetof(struct write_buffer, udp_address[0]))
#define SIZEOF_UDPBUFFER (sizeof(struct write_buffer))

//写缓冲区列表 数据结构定义
struct wb_list {
	struct write_buffer * head;//头
	struct write_buffer * tail;//尾
};

// 应用层的socket数据结构定义
struct socket {
	uintptr_t opaque;		//在skynet中用于保存服务的handle
	struct wb_list high;	//高
	struct wb_list low;		//低
	int64_t wb_size;		//发送缓冲区未发送的数据
	int fd;					//对应内核分配的fd
	int id;					//应用层维护的一个与fd对应的id 实际上是在socket池中的id
	uint16_t protocol;		//协议类型
	uint16_t type;			//socket类型或者状态
	union {
		int size;	//下一次read操作要分配的缓冲区大小?
		uint8_t udp_address[UDP_ADDRESS_SIZE];
	} p;
};

//socket服务器数据结构定义
struct socket_server {
	//管道相关，用于控制命令的传输
	int recvctrl_fd;	//读取控制文件描述符
	int sendctrl_fd;	//写入控制文件描述符
	int checkctrl;		//是否检查控制


	poll_fd event_fd;	//事件池文件描述符
	int alloc_id;		//应用层分配id用的,得到id再hash得到slot的索引
	int event_n;		//事件数
	int event_index;	//事件索引x
	struct socket_object_interface soi;		//套接字对象接口
	struct event ev[MAX_EVENT];		//存储已准备好读写的应用层事件	MAX_EVENT:64
	struct socket slot[MAX_SOCKET];//槽，用于存储应用层套接字	MAX_SOCKET:65536
	char buffer[MAX_INFO];		//缓冲区	MAX_INFO:128
	uint8_t udpbuffer[MAX_UDP_PACKAGE];	//udp缓冲区		MAX_UDP_PACKAGE:65535
	fd_set rfds;	//select的描述符集，用于判断管道是否有控制命令
};

// 以下结构用于控制包体结构
struct request_open {
	int id;
	int port;
	uintptr_t opaque;
	char host[1];
};

struct request_send {
	int id;
	int sz;
	char * buffer;
};

struct request_send_udp {
	struct request_send send;
	uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_setudp {
	int id;
	uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_close {
	int id;
	uintptr_t opaque;
};

struct request_listen {
	int id;
	int fd;
	uintptr_t opaque;
	char host[1];
};

struct request_bind {
	int id;
	int fd;
	uintptr_t opaque;
};

struct request_start {
	int id;
	uintptr_t opaque;
};

struct request_setopt {
	int id;
	int what;
	int value;
};

struct request_udp {
	int id;
	int fd;
	int family;
	uintptr_t opaque;
};

/*
	The first byte is TYPE  第一个字节为类型

	S Start socket
	B Bind socket
	L Listen socket
	K Close socket
	O Connect to (Open)
	X Exit
	D Send package (high)
	P Send package (low)
	A Send UDP package
	T Set opt
	U Create UDP socket
	C set udp address
 */

// 控制命令请求包
struct request_package {
	uint8_t header[8];	// 6 bytes dummy 头部 6个字节未用 [0-5] [6]是type [7]长度 长度指的是包体的长度
	union {
		char buffer[256];
		struct request_open open;
		struct request_send send;
		struct request_send_udp send_udp;
		struct request_close close;
		struct request_listen listen;
		struct request_bind bind;
		struct request_start start;
		struct request_setopt setopt;
		struct request_udp udp;
		struct request_setudp set_udp;
	} u;
	uint8_t dummy[256];
};

union sockaddr_all {
	struct sockaddr s;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

struct send_object {
	void * buffer;
	int sz;
	void (*free_func)(void *);
};

#define MALLOC skynet_malloc
#define FREE skynet_free

static inline bool
send_object_init(struct socket_server *ss, struct send_object *so, void *object, int sz) {
	if (sz < 0) {
		so->buffer = ss->soi.buffer(object);
		so->sz = ss->soi.size(object);
		so->free_func = ss->soi.free;
		return true;
	} else {
		so->buffer = object;
		so->sz = sz;
		so->free_func = FREE;
		return false;
	}
}

static inline void
write_buffer_free(struct socket_server *ss, struct write_buffer *wb) {
	if (wb->userobject) {
		ss->soi.free(wb->buffer);
	} else {
		FREE(wb->buffer);
	}
	FREE(wb);
}

static void
socket_keepalive(int fd) {
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));  
}

//预留一个ID
static int
reserve_id(struct socket_server *ss) {
	int i;
	for (i=0;i<MAX_SOCKET;i++) {
		int id = __sync_add_and_fetch(&(ss->alloc_id), 1);//无锁自增
		if (id < 0) {
			id = __sync_and_and_fetch(&(ss->alloc_id), 0x7fffffff);
		}
		struct socket *s = &ss->slot[HASH_ID(id)];//得到槽内的socket
		if (s->type == SOCKET_TYPE_INVALID) {
			if (__sync_bool_compare_and_swap(&s->type, SOCKET_TYPE_INVALID, SOCKET_TYPE_RESERVE)) {//重置类型
				s->id = id;//设置id
				s->fd = -1;//清空fd
				return id;
			} else {
				// retry 重试
				--i;
			}
		}
	}
	return -1;
}

static inline void
clear_wb_list(struct wb_list *list) {
	list->head = NULL;//设置头为空
	list->tail = NULL;//设置尾为空
}

//创建socket服务器
struct socket_server * 
socket_server_create() {
	int i;
	int fd[2];

	//efd:event fd
	poll_fd efd = sp_create();//创建socket poll,socket poll可以看作是一个事件池，不做具体的IO，只是把要监听的fd添加到事件池中进行IO事件（可读或可写）的监听
	if (sp_invalid(efd)) {//获取的poll_fd无效
		fprintf(stderr, "socket-server: create event pool failed.\n");
		return NULL;
	}
	if (pipe(fd)) {//建立管道，管道用于控制命令的传输
		//fd[0]为管道里的读取端
		//fd[1]为管道里的写入端
		sp_release(efd);//释放socket poll
		fprintf(stderr, "socket-server: create socket pair failed.\n");//创建套接字对失败
		return NULL;
	}
	if (sp_add(efd, fd[0], NULL)) {
		// add recvctrl_fd to event poll
		//添加fd[0]（读取端）到事件池中
		fprintf(stderr, "socket-server: can't add server fd to event pool.\n");
		close(fd[0]);//关闭fd[0]
		close(fd[1]);//关闭fd[1]
		sp_release(efd);//释放socket poll
		return NULL;
	}

	struct socket_server *ss = MALLOC(sizeof(*ss));//为socket server分配内存
	ss->event_fd = efd;//事件池文件描述符
	ss->recvctrl_fd = fd[0];//管道读取端fd
	ss->sendctrl_fd = fd[1];//管道写入端fd
	ss->checkctrl = 1;//是否检查控制，默认为true

	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];//取存储套接字槽的地址
		s->type = SOCKET_TYPE_INVALID;//类型初始化为无效
		clear_wb_list(&s->high);//清除写缓冲（高）列表
		clear_wb_list(&s->low);//清除写缓冲（低）列表
	}
	//初始化字段
	ss->alloc_id = 0;
	ss->event_n = 0;
	ss->event_index = 0;
	memset(&ss->soi, 0, sizeof(ss->soi));
	FD_ZERO(&ss->rfds);//将set清零使集合中不含任何fd
	assert(ss->recvctrl_fd < FD_SETSIZE);

	return ss;
}

static void
free_wb_list(struct socket_server *ss, struct wb_list *list) {
	struct write_buffer *wb = list->head;
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		write_buffer_free(ss, tmp);
	}
	list->head = NULL;
	list->tail = NULL;
}

static void
force_close(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;
	if (s->type == SOCKET_TYPE_INVALID) {
		return;
	}
	assert(s->type != SOCKET_TYPE_RESERVE);
	free_wb_list(ss,&s->high);
	free_wb_list(ss,&s->low);
	if (s->type != SOCKET_TYPE_PACCEPT && s->type != SOCKET_TYPE_PLISTEN) {
		sp_del(ss->event_fd, s->fd);
	}
	if (s->type != SOCKET_TYPE_BIND) {
		close(s->fd);
	}
	s->type = SOCKET_TYPE_INVALID;
}

void 
socket_server_release(struct socket_server *ss) {
	int i;
	struct socket_message dummy;
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		if (s->type != SOCKET_TYPE_RESERVE) {
			force_close(ss, s , &dummy);
		}
	}
	close(ss->sendctrl_fd);
	close(ss->recvctrl_fd);
	sp_release(ss->event_fd);
	FREE(ss);
}

static inline void
check_wb_list(struct wb_list *s) {
	assert(s->head == NULL);
	assert(s->tail == NULL);
}

//新建fd,实际为设置上层socket的一些字段而已
static struct socket *
new_fd(struct socket_server *ss, int id, int fd, int protocol, uintptr_t opaque, bool add) {
	struct socket * s = &ss->slot[HASH_ID(id)];//取出应用层socket,之前通过reserve_id已经预留了
	assert(s->type == SOCKET_TYPE_RESERVE);//预留的类型必然是SOCKET_TYPE_RESERVE

	if (add) {//是否添加到事件池中
		if (sp_add(ss->event_fd, fd, s)) {
			s->type = SOCKET_TYPE_INVALID;
			return NULL;
		}
	}

	//设置应用层socket相关字段
	s->id = id;//应用层id
	s->fd = fd;//内核fd
	s->protocol = protocol;//协议类型
	s->p.size = MIN_READ_BUFFER;//设置最小读缓冲大小
	s->opaque = opaque;//请求方服务地址
	s->wb_size = 0;
	check_wb_list(&s->high);
	check_wb_list(&s->low);
	return s;
}

// return -1 when connecting
static int
open_socket(struct socket_server *ss, struct request_open * request, struct socket_message *result) {
	int id = request->id;

	//设置result
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;

	struct socket *ns;
	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	struct addrinfo *ai_ptr = NULL;

	//设置端口
	char port[16];
	sprintf(port, "%d", request->port);

	memset(&ai_hints, 0, sizeof( ai_hints ) );

	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;


	//getaddrinfo提供独立于协议的名称解析，它的作用是将网址和服务，转换为IP地址和端口号的。
	//比如说，当我们输入一个http://www.baidu.com之类的网址，getaddrinfo函数就会去DNS服务器上查找对应的IP地址，以及http服务所对应的端口号。
	//因为一个网址往往对应多个IP地址，所以getaddrinfo得输出参数res是一个addrinfo结构体类型的链表指针，而每个addrinfo都包含一个sockaddr结构体。
	//这些sockaddr结构体随后可由套接口函数直接使用，去尝试进行连接。
	status = getaddrinfo( request->host, port, &ai_hints, &ai_list );
	if ( status != 0 ) {
		goto _failed;
	}
	int sock= -1;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next ) {
		sock = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );//建立socket
		if ( sock < 0 ) {
			continue;
		}

		socket_keepalive(sock);
		sp_nonblocking(sock);//非阻塞模式


		status = connect( sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);//连接服务器
		if ( status != 0 && errno != EINPROGRESS) {
			close(sock);
			sock = -1;
			continue;
		}
		break;//连接成功则跳出循环，其他的不连接
	}

	if (sock < 0) {
		goto _failed;
	}

	ns = new_fd(ss, id, sock, PROTOCOL_TCP, request->opaque, true);//加入了epoll,异步connect
	if (ns == NULL) {
		close(sock);
		goto _failed;
	}

	if(status == 0) {//连接成功
		ns->type = SOCKET_TYPE_CONNECTED;//设置状态为已连接

		//
		struct sockaddr * addr = ai_ptr->ai_addr;
		void * sin_addr = (ai_ptr->ai_family == AF_INET) ? (void*)&((struct sockaddr_in *)addr)->sin_addr : (void*)&((struct sockaddr_in6 *)addr)->sin6_addr;
		if (inet_ntop(ai_ptr->ai_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
			result->data = ss->buffer;
		}
		
		freeaddrinfo( ai_list );
		return SOCKET_OPEN;
	} else {//连接不能马上建立成功
		ns->type = SOCKET_TYPE_CONNECTING; //设置状态为连接中
		sp_write(ss->event_fd, ns->fd, ns, true);
	}

	freeaddrinfo( ai_list );
	return -1;
_failed:
	freeaddrinfo( ai_list );
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
	return SOCKET_ERROR;
}

static int
send_list_tcp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	while (list->head) {
		struct write_buffer * tmp = list->head;
		for (;;) {
			int sz = write(s->fd, tmp->ptr, tmp->sz);
			if (sz < 0) {
				switch(errno) {
				case EINTR:
					continue;
				case EAGAIN:
					return -1;
				}
				force_close(ss,s, result);
				return SOCKET_CLOSE;
			}
			s->wb_size -= sz;
			if (sz != tmp->sz) {
				tmp->ptr += sz;
				tmp->sz -= sz;
				return -1;
			}
			break;
		}
		list->head = tmp->next;
		write_buffer_free(ss,tmp);
	}
	list->tail = NULL;

	return -1;
}

static socklen_t
udp_socket_address(struct socket *s, const uint8_t udp_address[UDP_ADDRESS_SIZE], union sockaddr_all *sa) {
	int type = (uint8_t)udp_address[0];
	if (type != s->protocol)
		return 0;
	uint16_t port = 0;
	memcpy(&port, udp_address+1, sizeof(uint16_t));
	switch (s->protocol) {
	case PROTOCOL_UDP:
		memset(&sa->v4, 0, sizeof(sa->v4));
		sa->s.sa_family = AF_INET;
		sa->v4.sin_port = port;
		memcpy(&sa->v4.sin_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v4.sin_addr));	// ipv4 address is 32 bits
		return sizeof(sa->v4);
	case PROTOCOL_UDPv6:
		memset(&sa->v6, 0, sizeof(sa->v6));
		sa->s.sa_family = AF_INET6;
		sa->v6.sin6_port = port;
		memcpy(&sa->v6.sin6_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v6.sin6_addr));	// ipv4 address is 128 bits
		return sizeof(sa->v6);
	}
	return 0;
}

static int
send_list_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	while (list->head) {
		struct write_buffer * tmp = list->head;
		union sockaddr_all sa;
		socklen_t sasz = udp_socket_address(s, tmp->udp_address, &sa);
		int err = sendto(s->fd, tmp->ptr, tmp->sz, 0, &sa.s, sasz);
		if (err < 0) {
			switch(errno) {
			case EINTR:
			case EAGAIN:
				return -1;
			}
			fprintf(stderr, "socket-server : udp (%d) sendto error %s.\n",s->id, strerror(errno));
			return -1;
/*			// ignore udp sendto error
			
			result->opaque = s->opaque;
			result->id = s->id;
			result->ud = 0;
			result->data = NULL;

			return SOCKET_ERROR;
*/
		}

		s->wb_size -= tmp->sz;
		list->head = tmp->next;
		write_buffer_free(ss,tmp);
	}
	list->tail = NULL;

	return -1;
}

static int
send_list(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	if (s->protocol == PROTOCOL_TCP) {
		return send_list_tcp(ss, s, list, result);
	} else {
		return send_list_udp(ss, s, list, result);
	}
}

static inline int
list_uncomplete(struct wb_list *s) {
	struct write_buffer *wb = s->head;
	if (wb == NULL)
		return 0;
	
	return (void *)wb->ptr != wb->buffer;
}

static void
raise_uncomplete(struct socket * s) {
	struct wb_list *low = &s->low;
	struct write_buffer *tmp = low->head;
	low->head = tmp->next;
	if (low->head == NULL) {
		low->tail = NULL;
	}

	// move head of low list (tmp) to the empty high list
	struct wb_list *high = &s->high;
	assert(high->head == NULL);

	tmp->next = NULL;
	high->head = high->tail = tmp;
}

/*
	Each socket has two write buffer list, high priority and low priority.

	1. send high list as far as possible.
	2. If high list is empty, try to send low list.
	3. If low list head is uncomplete (send a part before), move the head of low list to empty high list (call raise_uncomplete) .
	4. If two lists are both empty, turn off the event. (call check_close)
 */
static int
send_buffer(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	assert(!list_uncomplete(&s->low));
	// step 1
	if (send_list(ss,s,&s->high,result) == SOCKET_CLOSE) {
		return SOCKET_CLOSE;
	}
	if (s->high.head == NULL) {
		// step 2
		if (s->low.head != NULL) {
			if (send_list(ss,s,&s->low,result) == SOCKET_CLOSE) {
				return SOCKET_CLOSE;
			}
			// step 3
			if (list_uncomplete(&s->low)) {
				raise_uncomplete(s);
			}
		} else {
			// step 4
			sp_write(ss->event_fd, s->fd, s, false);

			if (s->type == SOCKET_TYPE_HALFCLOSE) {
				force_close(ss, s, result);
				return SOCKET_CLOSE;
			}
		}
	}

	return -1;
}

static struct write_buffer *
append_sendbuffer_(struct socket_server *ss, struct wb_list *s, struct request_send * request, int size, int n) {
	struct write_buffer * buf = MALLOC(size);
	struct send_object so;
	buf->userobject = send_object_init(ss, &so, request->buffer, request->sz);
	buf->ptr = (char*)so.buffer+n;
	buf->sz = so.sz - n;
	buf->buffer = request->buffer;
	buf->next = NULL;
	if (s->head == NULL) {
		s->head = s->tail = buf;
	} else {
		assert(s->tail != NULL);
		assert(s->tail->next == NULL);
		s->tail->next = buf;
		s->tail = buf;
	}
	return buf;
}

static inline void
append_sendbuffer_udp(struct socket_server *ss, struct socket *s, int priority, struct request_send * request, const uint8_t udp_address[UDP_ADDRESS_SIZE]) {
	struct wb_list *wl = (priority == PRIORITY_HIGH) ? &s->high : &s->low;
	struct write_buffer *buf = append_sendbuffer_(ss, wl, request, SIZEOF_UDPBUFFER, 0);
	memcpy(buf->udp_address, udp_address, UDP_ADDRESS_SIZE);
	s->wb_size += buf->sz;
}

static inline void
append_sendbuffer(struct socket_server *ss, struct socket *s, struct request_send * request, int n) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->high, request, SIZEOF_TCPBUFFER, n);
	s->wb_size += buf->sz;
}

static inline void
append_sendbuffer_low(struct socket_server *ss,struct socket *s, struct request_send * request) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->low, request, SIZEOF_TCPBUFFER, 0);
	s->wb_size += buf->sz;
}

static inline int
send_buffer_empty(struct socket *s) {
	return (s->high.head == NULL && s->low.head == NULL);
}

/*
	When send a package , we can assign the priority : PRIORITY_HIGH or PRIORITY_LOW

	If socket buffer is empty, write to fd directly.
		If write a part, append the rest part to high list. (Even priority is PRIORITY_LOW)
	Else append package to high (PRIORITY_HIGH) or low (PRIORITY_LOW) list.
 */
static int
send_socket(struct socket_server *ss, struct request_send * request, struct socket_message *result, int priority, const uint8_t *udp_address) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];
	struct send_object so;
	send_object_init(ss, &so, request->buffer, request->sz);
	if (s->type == SOCKET_TYPE_INVALID || s->id != id 
		|| s->type == SOCKET_TYPE_HALFCLOSE
		|| s->type == SOCKET_TYPE_PACCEPT) {
		so.free_func(request->buffer);
		return -1;
	}
	assert(s->type != SOCKET_TYPE_PLISTEN && s->type != SOCKET_TYPE_LISTEN);
	if (send_buffer_empty(s) && s->type == SOCKET_TYPE_CONNECTED) {
		if (s->protocol == PROTOCOL_TCP) {
			int n = write(s->fd, so.buffer, so.sz);
			if (n<0) {
				switch(errno) {
				case EINTR:
				case EAGAIN:
					n = 0;
					break;
				default:
					fprintf(stderr, "socket-server: write to %d (fd=%d) error :%s.\n",id,s->fd,strerror(errno));
					force_close(ss,s,result);
					return SOCKET_CLOSE;
				}
			}
			if (n == so.sz) {
				so.free_func(request->buffer);
				return -1;
			}
			append_sendbuffer(ss, s, request, n);	// add to high priority list, even priority == PRIORITY_LOW
		} else {
			// udp
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;
			}
			union sockaddr_all sa;
			socklen_t sasz = udp_socket_address(s, udp_address, &sa);
			int n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);
			if (n != so.sz) {
				append_sendbuffer_udp(ss,s,priority,request,udp_address);
			} else {
				so.free_func(request->buffer);
			}
		}
		sp_write(ss->event_fd, s->fd, s, true);
	} else {
		if (s->protocol == PROTOCOL_TCP) {
			if (priority == PRIORITY_LOW) {
				append_sendbuffer_low(ss, s, request);
			} else {
				append_sendbuffer(ss, s, request, 0);
			}
		} else {
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;
			}
			append_sendbuffer_udp(ss,s,priority,request,udp_address);
		}
	}
	return -1;
}

static int
listen_socket(struct socket_server *ss, struct request_listen * request, struct socket_message *result) {
	int id = request->id;//上层ID
	int listen_fd = request->fd;//内核fd
	struct socket *s = new_fd(ss, id, listen_fd, PROTOCOL_TCP, request->opaque, false);
	if (s == NULL) {
		goto _failed;
	}
	s->type = SOCKET_TYPE_PLISTEN;//设置类型为待监听
	return -1;
_failed://失败处理
	close(listen_fd);
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;

	return SOCKET_ERROR;
}

static int
close_socket(struct socket_server *ss, struct request_close *request, struct socket_message *result) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id != id) {
		result->id = id;
		result->opaque = request->opaque;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_CLOSE;
	}
	if (!send_buffer_empty(s)) { 
		int type = send_buffer(ss,s,result);
		if (type != -1)
			return type;
	}
	if (send_buffer_empty(s)) {
		force_close(ss,s,result);
		result->id = id;
		result->opaque = request->opaque;
		return SOCKET_CLOSE;
	}
	s->type = SOCKET_TYPE_HALFCLOSE;

	return -1;
}

static int
bind_socket(struct socket_server *ss, struct request_bind *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	struct socket *s = new_fd(ss, id, request->fd, PROTOCOL_TCP, request->opaque, true);
	if (s == NULL) {
		result->data = NULL;
		return SOCKET_ERROR;
	}
	sp_nonblocking(request->fd);
	s->type = SOCKET_TYPE_BIND;
	result->data = "binding";
	return SOCKET_OPEN;
}

//启动socket
static int
start_socket(struct socket_server *ss, struct request_start *request, struct socket_message *result) {
	int id = request->id;//取出请求内的上层socket id

	//设置result的数据
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	result->data = NULL;


	struct socket *s = &ss->slot[HASH_ID(id)];//取出上层socket

	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {//如果socket的状态为无效 或者ID不匹配
		return SOCKET_ERROR;//返回出错
	}

	if (s->type == SOCKET_TYPE_PACCEPT || s->type == SOCKET_TYPE_PLISTEN) {//待接受或者待监听（未加入poll）

		if (sp_add(ss->event_fd, s->fd, s)) {//加入poll　　userdata存储的是socket本身
			//加入poll失败
			s->type = SOCKET_TYPE_INVALID;//设置类型为无效
			return SOCKET_ERROR;//返回出错
		}

		s->type = (s->type == SOCKET_TYPE_PACCEPT) ? SOCKET_TYPE_CONNECTED : SOCKET_TYPE_LISTEN;//重置设置状态,如果是待接收，则设置为已连接，如果是待监听，则设置为监听
		s->opaque = request->opaque;//

		result->data = "start";
		return SOCKET_OPEN;

	} else if (s->type == SOCKET_TYPE_CONNECTED) {
		s->opaque = request->opaque;

		result->data = "transfer";
		return SOCKET_OPEN;
	}
	return -1;
}

static void
setopt_socket(struct socket_server *ss, struct request_setopt *request) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return;
	}
	int v = request->value;
	setsockopt(s->fd, IPPROTO_TCP, request->what, &v, sizeof(v));
}

static void
block_readpipe(int pipefd, void *buffer, int sz) {
	for (;;) {//死循环
		int n = read(pipefd, buffer, sz);//从pipe中读取sz大小的数据到buffer中
		if (n<0) {
			if (errno == EINTR)//如果是由于信号中断，没读到任何数据
				continue;//继续读
			fprintf(stderr, "socket-server : read pipe error %s.\n",strerror(errno));//打印出错信息
			return;
		}
		// must atomic read from a pipe
		assert(n == sz);//读取的字节数一定要与传入的字节数(sz)相等
		return;
	}
}

//检查管道中是否有命令
static int
has_cmd(struct socket_server *ss) {
	struct timeval tv = {0,0};//select立即返回
	int retval;

	FD_SET(ss->recvctrl_fd, &ss->rfds);//将读fd加入set集合

	retval = select(ss->recvctrl_fd+1, &ss->rfds, NULL, NULL, &tv);
	if (retval == 1) {//描述符就绪
		return 1;
	}
	return 0;//出错或超时
}

static void
add_udp_socket(struct socket_server *ss, struct request_udp *udp) {
	int id = udp->id;
	int protocol;
	if (udp->family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		protocol = PROTOCOL_UDP;
	}
	struct socket *ns = new_fd(ss, id, udp->fd, protocol, udp->opaque, true);
	if (ns == NULL) {
		close(udp->fd);
		ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
	}
	ns->type = SOCKET_TYPE_CONNECTED;
	memset(ns->p.udp_address, 0, sizeof(ns->p.udp_address));
}

static int
set_udp_address(struct socket_server *ss, struct request_setudp *request, struct socket_message *result) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return -1;
	}
	int type = request->address[0];
	if (type != s->protocol) {
		// protocol mismatch
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		result->data = NULL;

		return SOCKET_ERROR;
	}
	if (type == PROTOCOL_UDP) {
		memcpy(s->p.udp_address, request->address, 1+2+4);	// 1 type, 2 port, 4 ipv4
	} else {
		memcpy(s->p.udp_address, request->address, 1+2+16);	// 1 type, 2 port, 16 ipv6
	}
	return -1;
}

// return type
static int
ctrl_cmd(struct socket_server *ss, struct socket_message *result) {
	int fd = ss->recvctrl_fd;//获取读取控制fd
	// the length of message is one byte, so 256+8 buffer size is enough.
	// 消息的长度是一个字节，所以缓冲区大小为256+8是足够的
	uint8_t buffer[256];//接收缓冲区
	uint8_t header[2];//头部
	block_readpipe(fd, header, sizeof(header));//阻塞从管道读头部
	int type = header[0];//消息类型
	int len = header[1];//消息长度
	block_readpipe(fd, buffer, len);//阻塞从管道读取指定长度的消息数据
	// ctrl command only exist in local fd, so don't worry about endian.
	// 控制命令仅存在于本地fd,所以不用担心大小端的问题


	//以下copy自文件上方数据结构定义
	/*
	The first byte is TYPE(第一个字节是类型)，第二个字节是消息长度
	
	S Start socket
	B Bind socket
	L Listen socket
	K Close socket
	O Connect to (Open)
	X Exit
	D Send package (high)
	P Send package (low)
	A Send UDP package
	T Set opt
	U Create UDP socket
	C set udp address
	*/

	//根据命令类型进行相应的处理
	switch (type) {
	case 'S':
		return start_socket(ss,(struct request_start *)buffer, result);
	case 'B':
		return bind_socket(ss,(struct request_bind *)buffer, result);
	case 'L':
		return listen_socket(ss,(struct request_listen *)buffer, result);
	case 'K':
		return close_socket(ss,(struct request_close *)buffer, result);
	case 'O'://打开一个socket请求
		return open_socket(ss, (struct request_open *)buffer, result);
	case 'X':
		result->opaque = 0;
		result->id = 0;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_EXIT;
	case 'D':
		return send_socket(ss, (struct request_send *)buffer, result, PRIORITY_HIGH, NULL);
	case 'P':
		return send_socket(ss, (struct request_send *)buffer, result, PRIORITY_LOW, NULL);
	case 'A': {
		struct request_send_udp * rsu = (struct request_send_udp *)buffer;
		return send_socket(ss, &rsu->send, result, PRIORITY_HIGH, rsu->address);
	}
	case 'C':
		return set_udp_address(ss, (struct request_setudp *)buffer, result);
	case 'T':
		setopt_socket(ss, (struct request_setopt *)buffer);
		return -1;
	case 'U':
		add_udp_socket(ss, (struct request_udp *)buffer);
		return -1;
	default:
		fprintf(stderr, "socket-server: Unknown ctrl %c.\n",type);//未知的控制
		return -1;
	};

	return -1;
}

// return -1 (ignore) when error
static int
forward_message_tcp(struct socket_server *ss, struct socket *s, struct socket_message * result) {
	int sz = s->p.size;
	char * buffer = MALLOC(sz);
	int n = (int)read(s->fd, buffer, sz);
	if (n<0) {
		FREE(buffer);
		switch(errno) {
		case EINTR:
			break;
		case EAGAIN:
			fprintf(stderr, "socket-server: EAGAIN capture.\n");
			break;
		default:
			// close when error
			force_close(ss, s, result);
			return SOCKET_ERROR;
		}
		return -1;
	}
	if (n==0) {
		FREE(buffer);
		force_close(ss, s, result);
		return SOCKET_CLOSE;
	}

	if (s->type == SOCKET_TYPE_HALFCLOSE) {
		// discard recv data
		FREE(buffer);
		return -1;
	}

	if (n == sz) {
		s->p.size *= 2;
	} else if (sz > MIN_READ_BUFFER && n*2 < sz) {
		s->p.size /= 2;
	}

	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = buffer;
	return SOCKET_DATA;
}

static int
gen_udp_address(int protocol, union sockaddr_all *sa, uint8_t * udp_address) {
	int addrsz = 1;
	udp_address[0] = (uint8_t)protocol;
	if (protocol == PROTOCOL_UDP) {
		memcpy(udp_address+addrsz, &sa->v4.sin_port, sizeof(sa->v4.sin_port));
		addrsz += sizeof(sa->v4.sin_port);
		memcpy(udp_address+addrsz, &sa->v4.sin_addr, sizeof(sa->v4.sin_addr));
		addrsz += sizeof(sa->v4.sin_addr);
	} else {
		memcpy(udp_address+addrsz, &sa->v6.sin6_port, sizeof(sa->v6.sin6_port));
		addrsz += sizeof(sa->v6.sin6_port);
		memcpy(udp_address+addrsz, &sa->v6.sin6_addr, sizeof(sa->v6.sin6_addr));
		addrsz += sizeof(sa->v6.sin6_addr);
	}
	return addrsz;
}

static int
forward_message_udp(struct socket_server *ss, struct socket *s, struct socket_message * result) {
	union sockaddr_all sa;
	socklen_t slen = sizeof(sa);
	int n = recvfrom(s->fd, ss->udpbuffer,MAX_UDP_PACKAGE,0,&sa.s,&slen);
	if (n<0) {
		switch(errno) {
		case EINTR:
		case EAGAIN:
			break;
		default:
			// close when error
			force_close(ss, s, result);
			return SOCKET_ERROR;
		}
		return -1;
	}
	uint8_t * data;
	if (slen == sizeof(sa.v4)) {
		if (s->protocol != PROTOCOL_UDP)
			return -1;
		data = MALLOC(n + 1 + 2 + 4);
		gen_udp_address(PROTOCOL_UDP, &sa, data + n);
	} else {
		if (s->protocol != PROTOCOL_UDPv6)
			return -1;
		data = MALLOC(n + 1 + 2 + 16);
		gen_udp_address(PROTOCOL_UDPv6, &sa, data + n);
	}
	memcpy(data, ss->udpbuffer, n);

	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = (char *)data;

	return SOCKET_UDP;
}

static int
report_connect(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	int error;
	socklen_t len = sizeof(error);  
	int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);  
	if (code < 0 || error) {  
		force_close(ss,s, result);
		return SOCKET_ERROR;
	} else {
		s->type = SOCKET_TYPE_CONNECTED;
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		if (send_buffer_empty(s)) {
			sp_write(ss->event_fd, s->fd, s, false);
		}
		union sockaddr_all u;
		socklen_t slen = sizeof(u);
		if (getpeername(s->fd, &u.s, &slen) == 0) {
			void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
			if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
				result->data = ss->buffer;
				return SOCKET_OPEN;
			}
		}
		result->data = NULL;
		return SOCKET_OPEN;
	}
}

// return 0 when failed
static int
report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	union sockaddr_all u;
	socklen_t len = sizeof(u);
	int client_fd = accept(s->fd, &u.s, &len);
	if (client_fd < 0) {
		return 0;
	}
	int id = reserve_id(ss);
	if (id < 0) {
		close(client_fd);
		return 0;
	}
	socket_keepalive(client_fd);
	sp_nonblocking(client_fd);
	struct socket *ns = new_fd(ss, id, client_fd, PROTOCOL_TCP, s->opaque, false);
	if (ns == NULL) {
		close(client_fd);
		return 0;
	}
	ns->type = SOCKET_TYPE_PACCEPT;
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = id;
	result->data = NULL;

	void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
	int sin_port = ntohs((u.s.sa_family == AF_INET) ? u.v4.sin_port : u.v6.sin6_port);
	char tmp[INET6_ADDRSTRLEN];
	if (inet_ntop(u.s.sa_family, sin_addr, tmp, sizeof(tmp))) {
		snprintf(ss->buffer, sizeof(ss->buffer), "%s:%d", tmp, sin_port);
		result->data = ss->buffer;
	}

	return 1;
}

//清理已关闭事件
static inline void 
clear_closed_event(struct socket_server *ss, struct socket_message * result, int type) {
	if (type == SOCKET_CLOSE || type == SOCKET_ERROR) {//如果是关闭或者出错
		int id = result->id;
		int i;
		for (i=ss->event_index; i<ss->event_n; i++) {
			struct event *e = &ss->ev[i];
			struct socket *s = e->s;
			if (s) {
				if (s->type == SOCKET_TYPE_INVALID && s->id == id) {
					e->s = NULL;//置空userdata
				}
			}
		}
	}
}

// return type
//返回类型
//socket服务器循环
//上层的API调用会转变为命令发送到管道，socket server则从管道读取命令进行处理
int 
socket_server_poll(struct socket_server *ss, struct socket_message * result, int * more) {
	for (;;) {//死循环
		if (ss->checkctrl) {//如果检查控制
			if (has_cmd(ss)) {//是否有命令
				int type = ctrl_cmd(ss, result);//读控制命令
				if (type != -1) {//返回了类型
					clear_closed_event(ss, result, type);
					return type;//返回类型
				} else//返回为－１，继续处理命令
					continue;
			} else {//如果没有命令，设置检查控制flag为false
				ss->checkctrl = 0;
			}
		}
		//刚开始两者都为0,或者已经处理完ＩＯ事件
		if (ss->event_index == ss->event_n) {
			ss->event_n = sp_wait(ss->event_fd, ss->ev, MAX_EVENT);//等待事件产生
			ss->checkctrl = 1;//设置检查控制flag为true
			if (more) {
				*more = 0;
			}
			ss->event_index = 0;//设置已处理事件索引
			if (ss->event_n <= 0) {
				ss->event_n = 0;
				return -1;
			}
		}
		//处理ＩＯ事件
		struct event *e = &ss->ev[ss->event_index++];//取出一个已准备好事件
		struct socket *s = e->s;//取出用户数据 （上层socket）
		if (s == NULL) {
			// dispatch pipe message at beginning
			//分发开头处的管道命令
			continue;
		}

		//根据socket类型进行处理
		switch (s->type) {
		case SOCKET_TYPE_CONNECTING:
			return report_connect(ss, s, result);
		case SOCKET_TYPE_LISTEN:
			if (report_accept(ss, s, result)) {
				return SOCKET_ACCEPT;
			} 
			break;
		case SOCKET_TYPE_INVALID:
			fprintf(stderr, "socket-server: invalid socket\n");
			break;
		default:
			if (e->read) {//可读
				int type;
				if (s->protocol == PROTOCOL_TCP) {
					type = forward_message_tcp(ss, s, result);
				} else {
					type = forward_message_udp(ss, s, result);
					if (type == SOCKET_UDP) {
						// try read again
						--ss->event_index;
						return SOCKET_UDP;
					}
				}
				if (e->write) {
					// Try to dispatch write message next step if write flag set.
					e->read = false;
					--ss->event_index;
				}
				if (type == -1)
					break;
				clear_closed_event(ss, result, type);
				return type;
			}
			if (e->write) {//可写
				int type = send_buffer(ss, s, result);
				if (type == -1)
					break;
				clear_closed_event(ss, result, type);
				return type;
			}
			break;
		}
	}
}

//向socket server发送请求，通过管道
static void
send_request(struct socket_server *ss, struct request_package *request, char type, int len) {
	//组装数据
	request->header[6] = (uint8_t)type;
	request->header[7] = (uint8_t)len;

	//发送数据
	for (;;) {
		int n = write(ss->sendctrl_fd, &request->header[6], len+2);//写入管道，管道也是一种独立的文件，网络也是
		if (n<0) {
			if (errno != EINTR) {
				fprintf(stderr, "socket-server : send ctrl command error %s.\n", strerror(errno));
			}
			continue;
		}
		assert(n == len+2);
		return;
	}
}

static int
open_request(struct socket_server *ss, struct request_package *req, uintptr_t opaque, const char *addr, int port) {
	int len = strlen(addr);
	if (len + sizeof(req->u.open) > 256) {
		fprintf(stderr, "socket-server : Invalid addr %s.\n",addr);
		return -1;
	}
	int id = reserve_id(ss);//预留一个  socket id 
	if (id < 0)
		return -1;

	//设置相应字段
	req->u.open.opaque = opaque;
	req->u.open.id = id;
	req->u.open.port = port;
	memcpy(req->u.open.host, addr, len);
	req->u.open.host[len] = '\0';

	return len;
}

int 
socket_server_connect(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {//opaque实际上是上下文的句柄(handler,也叫source)
	struct request_package request;//定义一个请求包
	int len = open_request(ss, &request, opaque, addr, port);//组装启动请求包
	if (len < 0)
		return -1;
	send_request(ss, &request, 'O', sizeof(request.u.open) + len);//发送请求包
	return request.u.open.id;
}

// return -1 when error
int64_t 
socket_server_send(struct socket_server *ss, int id, const void * buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		return -1;
	}

	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	send_request(ss, &request, 'D', sizeof(request.u.send));
	return s->wb_size;
}

void 
socket_server_send_lowpriority(struct socket_server *ss, int id, const void * buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		return;
	}

	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	send_request(ss, &request, 'P', sizeof(request.u.send));
}

void
socket_server_exit(struct socket_server *ss) {
	struct request_package request;
	send_request(ss, &request, 'X', 0);
}

void
socket_server_close(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.opaque = opaque;
	send_request(ss, &request, 'K', sizeof(request.u.close));
}

// return -1 means failed
// or return AF_INET or AF_INET6
static int
do_bind(const char *host, int port, int protocol, int *family) {
	int fd;
	int status;
	int reuse = 1;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];
	if (host == NULL || host[0] == 0) {
		host = "0.0.0.0";	// INADDR_ANY
	}
	sprintf(portstr, "%d", port);
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	if (protocol == IPPROTO_TCP) {
		ai_hints.ai_socktype = SOCK_STREAM;
	} else {
		assert(protocol == IPPROTO_UDP);
		ai_hints.ai_socktype = SOCK_DGRAM;
	}
	ai_hints.ai_protocol = protocol;

	status = getaddrinfo( host, portstr, &ai_hints, &ai_list );
	if ( status != 0 ) {
		return -1;
	}
	*family = ai_list->ai_family;
	fd = socket(*family, ai_list->ai_socktype, 0);//创建socket
	if (fd < 0) {
		goto _failed_fd;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int))==-1) {
		goto _failed;
	}
	status = bind(fd, (struct sockaddr *)ai_list->ai_addr, ai_list->ai_addrlen);//命名socket
	if (status != 0)
		goto _failed;

	freeaddrinfo( ai_list );
	return fd;
_failed:
	close(fd);
_failed_fd:
	freeaddrinfo( ai_list );
	return -1;
}

static int
do_listen(const char * host, int port, int backlog) {
	int family = 0;
	int listen_fd = do_bind(host, port, IPPROTO_TCP, &family); //创建、命名socket
	if (listen_fd < 0) {
		return -1;
	}
	if (listen(listen_fd, backlog) == -1) {//监听socket
		close(listen_fd);
		return -1;
	}
	return listen_fd;
}

int 
socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char * addr, int port, int backlog) {

	//监听完socket，就把命令发到socket server，然后等待异步回应
	int fd = do_listen(addr, port, backlog);
	if (fd < 0) {
		return -1;
	}
	struct request_package request;
	int id = reserve_id(ss);
	if (id < 0) 
	{		
		close(fd);
		return id;
	}
	request.u.listen.opaque = opaque;//请求方服务句柄（地址）
	request.u.listen.id = id;//内部预留的socket ID
	request.u.listen.fd = fd;//监听生成的fd 
	send_request(ss, &request, 'L', sizeof(request.u.listen));//发送监听请求
	return id;
}

int
socket_server_bind(struct socket_server *ss, uintptr_t opaque, int fd) {
	struct request_package request;
	int id = reserve_id(ss);
	if (id < 0)
		return -1;
	request.u.bind.opaque = opaque;
	request.u.bind.id = id;
	request.u.bind.fd = fd;
	send_request(ss, &request, 'B', sizeof(request.u.bind));
	return id;
}

void 
socket_server_start(struct socket_server *ss, uintptr_t opaque, int id) {

	//opaque为调用方的地址，id为listen返回的id
	struct request_package request;
	request.u.start.id = id;
	request.u.start.opaque = opaque;
	send_request(ss, &request, 'S', sizeof(request.u.start));
}

void
socket_server_nodelay(struct socket_server *ss, int id) {
	struct request_package request;
	request.u.setopt.id = id;
	request.u.setopt.what = TCP_NODELAY;
	request.u.setopt.value = 1;
	send_request(ss, &request, 'T', sizeof(request.u.setopt));
}

void 
socket_server_userobject(struct socket_server *ss, struct socket_object_interface *soi) {
	ss->soi = *soi;
}

// UDP

int 
socket_server_udp(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	int fd;
	int family;
	if (port != 0 || addr != NULL) {
		// bind
		fd = do_bind(addr, port, IPPROTO_UDP, &family);
		if (fd < 0) {
			return -1;
		}
	} else {
		family = AF_INET;
		fd = socket(family, SOCK_DGRAM, 0);
		if (fd < 0) {
			return -1;
		}
	}
	sp_nonblocking(fd);

	int id = reserve_id(ss);
	if (id < 0) {
		close(fd);
		return -1;
	}
	struct request_package request;
	request.u.udp.id = id;
	request.u.udp.fd = fd;
	request.u.udp.opaque = opaque;
	request.u.udp.family = family;

	send_request(ss, &request, 'U', sizeof(request.u.udp));	
	return id;
}

int64_t 
socket_server_udp_send(struct socket_server *ss, int id, const struct socket_udp_address *addr, const void *buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		return -1;
	}

	struct request_package request;
	request.u.send_udp.send.id = id;
	request.u.send_udp.send.sz = sz;
	request.u.send_udp.send.buffer = (char *)buffer;

	const uint8_t *udp_address = (const uint8_t *)addr;
	int addrsz;
	switch (udp_address[0]) {
	case PROTOCOL_UDP:
		addrsz = 1+2+4;		// 1 type, 2 port, 4 ipv4
		break;
	case PROTOCOL_UDPv6:
		addrsz = 1+2+16;	// 1 type, 2 port, 16 ipv6
		break;
	default:
		return -1;
	}

	memcpy(request.u.send_udp.address, udp_address, addrsz);	

	send_request(ss, &request, 'A', sizeof(request.u.send_udp.send)+addrsz);
	return s->wb_size;
}

int
socket_server_udp_connect(struct socket_server *ss, int id, const char * addr, int port) {
	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];
	sprintf(portstr, "%d", port);
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_DGRAM;
	ai_hints.ai_protocol = IPPROTO_UDP;

	status = getaddrinfo(addr, portstr, &ai_hints, &ai_list );
	if ( status != 0 ) {
		return -1;
	}
	struct request_package request;
	request.u.set_udp.id = id;
	int protocol;

	if (ai_list->ai_family == AF_INET) {
		protocol = PROTOCOL_UDP;
	} else if (ai_list->ai_family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		freeaddrinfo( ai_list );
		return -1;
	}

	int addrsz = gen_udp_address(protocol, (union sockaddr_all *)ai_list->ai_addr, request.u.set_udp.address);

	freeaddrinfo( ai_list );

	send_request(ss, &request, 'C', sizeof(request.u.set_udp) - sizeof(request.u.set_udp.address) +addrsz);

	return 0;
}

const struct socket_udp_address *
socket_server_udp_address(struct socket_server *ss, struct socket_message *msg, int *addrsz) {
	uint8_t * address = (uint8_t *)(msg->data + msg->ud);
	int type = address[0];
	switch(type) {
	case PROTOCOL_UDP:
		*addrsz = 1+2+4;
		break;
	case PROTOCOL_UDPv6:
		*addrsz = 1+2+16;
		break;
	default:
		return NULL;
	}
	return (const struct socket_udp_address *)address;
}
