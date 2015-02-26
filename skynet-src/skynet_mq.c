//消息队列
//二级消息队列
#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_QUEUE_SIZE 64	//默认队列的大小为64
#define MAX_GLOBAL_MQ 0x10000	//64K,单机服务上限是64K，因而global mq数量最大值也是64k

// 0 means mq is not in global mq.	0意味着mq不在全局的mq
// 1 means mq is in global mq , or the message is dispatching.	1意味着mq在全局的mq里，或者消息正在派发

#define MQ_IN_GLOBAL 1
#define MQ_OVERLOAD 1024

//消息队列数据结构定义
struct message_queue {
	uint32_t handle;//句柄号
	int cap;		//容量
	int head;		//队头
	int tail;		//队尾
	int lock;		//用于实现自旋锁 加锁将这个值设置为1 释放锁将这个值设置为0
	int release;	//消息队列释放标记，当要释放一个服务的时候 清理标记
	int in_global;	//是否在全局队列中
	int overload;	//超载
	int overload_threshold;			//超载阀值
	struct skynet_message *queue;	//具体的消息队列(存放具体的skynet消息) 类似于数组
	struct message_queue *next;		//下一个消息队列
};

//全局队列数据结构定义
struct global_queue {
	struct message_queue *head;	//消息队列头指针
	struct amessage_queue *til;	//消息队列尾指针
	int lock;					//锁
};

static struct global_queue *Q = NULL;	//全局队列引用

#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {}	//自旋锁 加锁
#define UNLOCK(q) __sync_lock_release(&(q)->lock);					//解锁

//向全局队列push新的消息队列
//在队尾push
void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q;//获得全局队列引用

	LOCK(q)//加锁
	assert(queue->next == NULL);//新push进全局队列的消息队列的next指针必为空
	if(q->tail) {//如果全局队列的队尾指针不为空(此时队列不为空)
		q->tail->next = queue;//队尾指针指向的消息队列的next指针指向传入的消息队列
		q->tail = queue;//将队尾指针指向传入的消息队列
	} else {
		q->head = q->tail = queue;//第一个元素(此时队列为空)，队头队尾都指向这个队列
	}
	UNLOCK(q)//解锁
}

//从全局队列pop出消息队列
//在队头pop
struct message_queue * 
skynet_globalmq_pop() {
	struct global_queue *q = Q;//获得全局队列引用

	LOCK(q)//加锁
	struct message_queue *mq = q->head;//先将要返回的指针指向队头
	if(mq) {//如果队头不为空
		q->head = mq->next;//将队头指向队列第二个元素
		if(q->head == NULL) {//如果此时队头为空了，代表当前队列只有一个元素，则队头指针和队尾指针都指向该元素
			assert(mq == q->tail);//断言队尾指针指向当前唯一元素
			q->tail = NULL;//如果只有一个元素pop出去了，那么队尾指针指向也应该置为空，因为没元素了啊
		}
		mq->next = NULL;//将要返回的消息队列的next置为空
	}
	UNLOCK(q)//解锁

	return mq;//返回消息队列
}

struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q));//为消息队列分配内存
	q->handle = handle;//将句柄号存入消息队列的handle字段中
	q->cap = DEFAULT_QUEUE_SIZE;//设置队列容量
	q->head = 0;//队头
	q->tail = 0;//队尾
	q->lock = 0;//自旋锁
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_force_push to push it to global queue.
	//当消息队列创建时(总是在服务创建和服务初始化之间)
	//设置in_global标志为1避免将它push到全局队列中
	//如果服务初始化成功了，skynet_context_new将会调用skynet_mq_force_push将它push到全局队列中(是skynet_globalmq_push吧)
	q->in_global = MQ_IN_GLOBAL;
	q->release = 0;//释放标记
	q->overload = 0;//超载标记
	q->overload_threshold = MQ_OVERLOAD;//超载阀值
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap);//为具体的消息队列分配内存
	q->next = NULL;//下一个消息队列指针

	return q;
}

//释放消息队列
static void 
_release(struct message_queue *q) {
	assert(q->next == NULL);//断言传入的队列的next指针必为空
	skynet_free(q->queue);//释放具体的消息队列
	skynet_free(q);//释放消息队列
}

//获取消息队列的句柄号
uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle;//直接返回字段即可
}

//计算消息队列的长度
int
skynet_mq_length(struct message_queue *q) {
	int head, tail,cap;

	LOCK(q)//锁住该队列
	head = q->head;//得到头
	tail = q->tail;//得到尾
	cap = q->cap;//得到容量
	UNLOCK(q)//解锁该队列
	
	if (head <= tail) {
		return tail - head;
	}
	return tail + cap - head;
}

//
int
skynet_mq_overload(struct message_queue *q) {
	if (q->overload) {
		int overload = q->overload;
		q->overload = 0;
		return overload;
	} 
	return 0;
}

//从消息队列POP出消息
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;
	LOCK(q)

	if (q->head != q->tail) {
		*message = q->queue[q->head++];
		ret = 0;
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

		if (head >= cap) {
			q->head = head = 0;
		}
		int length = tail - head;
		if (length < 0) {
			length += cap;
		}
		while (length > q->overload_threshold) {
			q->overload = length;
			q->overload_threshold *= 2;
		}
	} else {
		// reset overload_threshold when queue is empty
		q->overload_threshold = MQ_OVERLOAD;
	}

	if (ret) {
		q->in_global = 0;
	}
	
	UNLOCK(q)

	return ret;
}

//扩充消息队列
static void
expand_queue(struct message_queue *q) {
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	for (i=0;i<q->cap;i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;
	
	skynet_free(q->queue);
	q->queue = new_queue;
}

//向消息队列PUSH消息
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);
	LOCK(q)

	q->queue[q->tail] = *message;
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	if (q->head == q->tail) {
		expand_queue(q);
	}

	if (q->in_global == 0) {
		q->in_global = MQ_IN_GLOBAL;
		skynet_globalmq_push(q);
	}
	
	UNLOCK(q)
}

void 
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q));//为全局队列分配内存
	memset(q,0,sizeof(*q));//清空内存
	Q=q;//保存分配的指针
}

//标记消息队列将要释放
void 
skynet_mq_mark_release(struct message_queue *q) {
	LOCK(q)
	assert(q->release == 0);
	q->release = 1;
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}
	UNLOCK(q)
}

//丢弃队列
static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	while(!skynet_mq_pop(q, &msg)) {
		drop_func(&msg, ud);
	}
	_release(q);
}

//释放消息队列
void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	LOCK(q)
	
	if (q->release) {
		UNLOCK(q)
		_drop_queue(q, drop_func, ud);
	} else {
		skynet_globalmq_push(q);
		UNLOCK(q)
	}
}
