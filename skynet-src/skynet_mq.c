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
#define MQ_OVERLOAD 1024	//超载阀值

//消息队列数据结构定义 
struct message_queue {
	uint32_t handle;//句柄号
	int cap;		//容量
	int head;		//队头索引
	int tail;		//队尾索引
	int lock;		//用于实现自旋锁 加锁将这个值设置为1 释放锁将这个值设置为0
	int release;	//消息队列释放标记，当要释放一个服务的时候 清理标记
	int in_global;	//是否在全局队列中
	int overload;	//超载值（超载队列的长度）
	int overload_threshold;			//超载阀值
	struct skynet_message *queue;	//具体的消息队列(存放具体的skynet消息) 类似于数组，按需扩展
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
	
	if (head <= tail) {//如果头小于等于尾 没有回绕
		return tail - head;//直接返回二者差值即为当前消息队列的长度
	}
	return tail + cap - head;//发生回绕了
}

//获取消息队列的超载值，如果超载了，返回后，将重置超载值
//返回的是当前超载的队列长度
int 
skynet_mq_overload(struct message_queue *q) {
	if (q->overload) {//如果消息队列超载了
		int overload = q->overload;//取出超载值
		q->overload = 0;//重置超载值
		return overload;//返回取出的超载值
	} 
	return 0;//返回0代表消息队列未超载
}

//从消息队列POP出消息
//队头POP
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;//默认返回值为1
	LOCK(q)//加锁队列n

	if (q->head != q->tail) {//如果队列头不等于队列尾 代表队列中有消息
		*message = q->queue[q->head++];//根据当前的队头索引取出队列中的消息，并自增队头索引
		ret = 0;//将返回值设为0
		int head = q->head;//取得队头
		int tail = q->tail;//取得队尾
		int cap = q->cap;//取出容量

		if (head >= cap) {//如果队头索引超出了容量
			q->head = head = 0;//重置队头索引
		}
		int length = tail - head;//计算当前队列长度
		if (length < 0) {//如果长度小于0,则发生回绕了
			length += cap;//需要加上容量获取长度，即length=tail + cap - head
		}
		while (length > q->overload_threshold) {//当队列长度大于超载阀值时
			q->overload = length;//设置超载值为当前队列的长度
			q->overload_threshold *= 2;//增加队列的超载阀值为原有的2倍
			//如果当前队列的长度依然大于增加后的队列超载阀值，则继续这个过程
		}
	} else {//此时队列已经空了，为什么呢？因为如果head==tail，则有两种情况，一种是队列空，一种是队列满，
			//假设是队列满,那一定是经历过push消息的操作，但是在skynet_mq_push中，在push消息后，有判断是否队列满的情况，
			//而且如果满了，会进行队列的扩充操作，那么这个地方是不可能队列满的

		// reset overload_threshold when queue is empty
		// 当队列为空时，重置超载阀值
		q->overload_threshold = MQ_OVERLOAD;
	}

	if (ret) {//没有获取到消息
		q->in_global = 0;//设置是否在全局队列中为false
	}
	
	UNLOCK(q)//解锁队列

	return ret;//返回0或1	0代表获取到消息了，1代表没有获取到消息
}

//扩充消息队列
static void
expand_queue(struct message_queue *q) {
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);//重新分配2倍于原有队列容量的内存
	int i;
	for (i=0;i<q->cap;i++) {//遍历队列，因为此时队列已满
		new_queue[i] = q->queue[(q->head + i) % q->cap];//按照消息在原有队列的顺序拷贝到新的消息队列中，即使发生回绕了，但顺序不变
	}
	q->head = 0;//设置头为0
	q->tail = q->cap;//设置尾为原有队列的容量
	q->cap *= 2;//设置新的容量为原有容量的两倍
	
	skynet_free(q->queue);//释放原有的消息队列
	q->queue = new_queue;//使用新分配的消息队列
}

//向消息队列PUSH消息
//队尾PUSH
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);//向一个队列PUSH消息，传入的消息当然不能为空，先断言判断下
	LOCK(q)//加锁队列

	q->queue[q->tail] = *message;//将消息存储到当前队尾索引所在位置
	if (++ q->tail >= q->cap) {//先自增队尾索引，如果队尾索引超出了当前队列的容量
		q->tail = 0;//设置队尾索引到0  回绕了
	}

	if (q->head == q->tail) {//如果队列头索引等于队列尾了，则代表没有位置存储元素了,消息队列已经满了
		//队列头等于队列尾肯定不是初始状态：即head和tail都等于0，因为上面几句代码已经导致队尾指针增加了
		//那么此时肯定是发生回绕了
		//一种情况是一直push消息，直到发生回绕，此时head和tail都等于0
		//另一种情况是队列有push消息，也有pop消息，但push更快，则tail追上了head，此时head和tail的值为多少不确定

		expand_queue(q);//需要扩充队列了
	}

	if (q->in_global == 0) {//如果消息队列不在全局队列中
		q->in_global = MQ_IN_GLOBAL;//设置标记
		skynet_globalmq_push(q);//将消息队列push到全局队列中
	}
	
	UNLOCK(q)//解锁队列
}

void 
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q));//为全局队列分配内存
	memset(q,0,sizeof(*q));//清空内存
	Q=q;//保存分配的指针
}

//标记消息队列释放标记
void 
skynet_mq_mark_release(struct message_queue *q) {
	LOCK(q)//加锁队列
	assert(q->release == 0);//断言当前的释放标记为false
	q->release = 1;//设置释放标记为true
	if (q->in_global != MQ_IN_GLOBAL) {//如果当前消息队列不在全局队列中
		skynet_globalmq_push(q);//将它push到全局队列中
	}
	UNLOCK(q)//解锁队列
}

//丢弃队列
static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	while(!skynet_mq_pop(q, &msg)) {//不断从队列中pop出消息直到没有消息
		drop_func(&msg, ud);//将消息传入给丢弃函数
	}
	_release(q);//最终释放队列
}

//释放消息队列
void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	LOCK(q)//先加锁队列
	
	if (q->release) {//如果释放标记为true，才会释放队列
		UNLOCK(q)//解锁队列
		_drop_queue(q, drop_func, ud);//丢弃队列
	} else {//如果释放标记为false
		skynet_globalmq_push(q);//将消息队列push到全局队列中
		UNLOCK(q)//解锁队列
	}
}
