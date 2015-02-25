//服务编号管理
#include "skynet.h"

#include "skynet_handle.h"
#include "skynet_server.h"
#include "rwlock.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define DEFAULT_SLOT_SIZE 4
#define MAX_SLOT_SIZE 0x40000000

//每个模块（模块被称为服务）都有一个永不重复（即使模块退出）的数字 id，这个概念叫做handle,类似windows内的句柄概念

//句柄名字数据结构定义
struct handle_name {
	char * name;		//名字
	uint32_t handle;	//句柄
};
//句柄存储数据结构定义
struct handle_storage {
	struct rwlock lock;	//读写锁

	uint32_t harbor;	//节点号
	uint32_t handle_index;//句柄索引
	int slot_size;//槽大小
	struct skynet_context ** slot;//槽
	
	int name_cap;//名字容量
	int name_count;//名字计数
	struct handle_name *name;//句柄名字
};

static struct handle_storage *H = NULL;

//注册句柄(取得一个句柄)
uint32_t
skynet_handle_register(struct skynet_context *ctx) {
	struct handle_storage *s = H;//获取句柄存储引用

	rwlock_wlock(&s->lock);//写加锁
	
	for (;;) {//死循环
		int i;
		for (i=0;i<s->slot_size;i++) {//遍历槽
			uint32_t handle = (i+s->handle_index) & HANDLE_MASK;//获取句柄值
			int hash = handle & (s->slot_size-1);//获取hash值，作为槽的索引
			if (s->slot[hash] == NULL) {//槽内没有存储skynet上下文
				s->slot[hash] = ctx;//将skynet上下文存储到槽内
				s->handle_index = handle + 1;//句柄索引+1

				rwlock_wunlock(&s->lock);//写解锁

				handle |= s->harbor;//句柄或上节点号
				return handle;//返回句柄
			}
		}
		//槽不够用了，重新分配
		assert((s->slot_size*2 - 1) <= HANDLE_MASK);
		struct skynet_context ** new_slot = skynet_malloc(s->slot_size * 2 * sizeof(struct skynet_context *));
		memset(new_slot, 0, s->slot_size * 2 * sizeof(struct skynet_context *));
		for (i=0;i<s->slot_size;i++) {
			int hash = skynet_context_handle(s->slot[i]) & (s->slot_size * 2 - 1);
			assert(new_slot[hash] == NULL);
			new_slot[hash] = s->slot[i];
		}
		skynet_free(s->slot);
		s->slot = new_slot;
		s->slot_size *= 2;
	}
}

//句柄回收
int
skynet_handle_retire(uint32_t handle) {
	int ret = 0;
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock);

	uint32_t hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];

	if (ctx != NULL && skynet_context_handle(ctx) == handle) {
		skynet_context_release(ctx);
		s->slot[hash] = NULL;
		ret = 1;
		int i;
		int j=0, n=s->name_count;
		for (i=0; i<n; ++i) {
			if (s->name[i].handle == handle) {
				skynet_free(s->name[i].name);
				continue;
			} else if (i!=j) {
				s->name[j] = s->name[i];
			}
			++j;
		}
		s->name_count = j;
	}

	rwlock_wunlock(&s->lock);

	return ret;
}

void 
skynet_handle_retireall() {
	struct handle_storage *s = H;
	for (;;) {
		int n=0;
		int i;
		for (i=0;i<s->slot_size;i++) {
			rwlock_rlock(&s->lock);
			struct skynet_context * ctx = s->slot[i];
			uint32_t handle = 0;
			if (ctx)
				handle = skynet_context_handle(ctx);
			rwlock_runlock(&s->lock);
			if (handle != 0) {
				if (skynet_handle_retire(handle)) {
					++n;
				}
			}
		}
		if (n==0)
			return;
	}
}

//使用句柄获取上下文引用
struct skynet_context * 
skynet_handle_grab(uint32_t handle) {
	struct handle_storage *s = H;//句柄存储引用
	struct skynet_context * result = NULL;//返回结果指针，指向上下文

	rwlock_rlock(&s->lock);//读加锁

	uint32_t hash = handle & (s->slot_size-1);//获取哈希值
	struct skynet_context * ctx = s->slot[hash];//从槽内获取上下文引用
	if (ctx && skynet_context_handle(ctx) == handle) {//获取到了上下文，并且该上下文的句柄就是传入的句柄
		result = ctx;//设置返回结果指针指向该上下文引用
		skynet_context_grab(result);//增加上下文的引用计数
	}

	rwlock_runlock(&s->lock);//读解锁

	return result;
}
//根据名字查找句柄
uint32_t 
skynet_handle_findname(const char * name) {
	struct handle_storage *s = H;//获取句柄存储引用

	rwlock_rlock(&s->lock);//读加锁

	uint32_t handle = 0;//句柄

	//二分法遍历
	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			handle = n->handle;
			break;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}

	rwlock_runlock(&s->lock);//读解锁

	return handle;//返回句柄
}

static void
_insert_name_before(struct handle_storage *s, char *name, uint32_t handle, int before) {
	if (s->name_count >= s->name_cap) {//扩充容量
		s->name_cap *= 2;
		assert(s->name_cap <= MAX_SLOT_SIZE);
		struct handle_name * n = skynet_malloc(s->name_cap * sizeof(struct handle_name));
		int i;
		for (i=0;i<before;i++) {
			n[i] = s->name[i];
		}
		for (i=before;i<s->name_count;i++) {
			n[i+1] = s->name[i];
		}
		skynet_free(s->name);
		s->name = n;
	} else {
		int i;
		for (i=s->name_count;i>before;i--) {
			s->name[i] = s->name[i-1];//将所有元素后移一格
		}
	}
	//用空出的新格子存储名字和句柄
	s->name[before].name = name;
	s->name[before].handle = handle;
	s->name_count ++;//计数加1
}

//插入名字
static const char *
_insert_name(struct handle_storage *s, const char * name, uint32_t handle) {
	int begin = 0;//起始位置
	int end = s->name_count - 1;//结束位置
	while (begin<=end) {//遍历 二分法
		int mid = (begin+end)/2;//中间位置
		struct handle_name *n = &s->name[mid];//取句柄名字指针
		int c = strcmp(n->name, name);//比较名字是否匹配
		if (c==0) {//名字匹配
			return NULL;//返回空
		}
		if (c<0) {//待插入的名字大于当前位置的名字
			begin = mid + 1;//移动begin到（当前位置+1）处
		} else {//待插入的名字小于当前位置的名字
			end = mid - 1;//移动end到（当前位置-1）处
		}
	}
	char * result = skynet_strdup(name);//复制名字

	_insert_name_before(s, result, handle, begin);

	return result;
}

//命名句柄
const char * 
skynet_handle_namehandle(uint32_t handle, const char *name) {
	rwlock_wlock(&H->lock);//写加锁

	const char * ret = _insert_name(H, name, handle);//插入名字

	rwlock_wunlock(&H->lock);//写解锁

	return ret;
}

void 
skynet_handle_init(int harbor) {
	assert(H==NULL);
	struct handle_storage * s = skynet_malloc(sizeof(*H));//分配内存
	s->slot_size = DEFAULT_SLOT_SIZE;//设置槽大小
	s->slot = skynet_malloc(s->slot_size * sizeof(struct skynet_context *));//为槽分配内存 其实是s->slot_size个指针
	memset(s->slot, 0, s->slot_size * sizeof(struct skynet_context *));//清空内存

	rwlock_init(&s->lock);//读写锁初始化
	// reserve 0 for system
	
	//初始化handle存储相关数据
	s->harbor = (uint32_t) (harbor & 0xff) << HANDLE_REMOTE_SHIFT;
	s->handle_index = 1;
	s->name_cap = 2;
	s->name_count = 0;
	s->name = skynet_malloc(s->name_cap * sizeof(struct handle_name));

	H = s;

	// Don't need to free H
}

