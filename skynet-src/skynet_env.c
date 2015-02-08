#include "skynet.h"
#include "skynet_env.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <assert.h>
//skynet环境数据结构
struct skynet_env {
	int lock;			//锁
	lua_State *L;		//LUA虚拟机
};

static struct skynet_env *E = NULL;

#define LOCK(q) while (__sync_lock_test_and_set(&(q)->lock,1)) {}//上锁	（用__sync_lock_test_and_set实现的自旋锁）
#define UNLOCK(q) __sync_lock_release(&(q)->lock);//解锁

const char * 
skynet_getenv(const char *key) {
	LOCK(E)

	lua_State *L = E->L;
	
	lua_getglobal(L, key);//把全局变量 key 里的值压入堆栈
	const char * result = lua_tostring(L, -1);//取出value
	lua_pop(L, 1);//弹出value

	UNLOCK(E)

	return result;
}

void 
skynet_setenv(const char *key, const char *value) {
	LOCK(E)
	
	lua_State *L = E->L;
	lua_getglobal(L, key);//把全局变量 key 里的值压入堆栈
	assert(lua_isnil(L, -1));//如果不为空的话
	lua_pop(L,1);//弹出nil
	lua_pushstring(L,value);//将value压栈
	lua_setglobal(L,key);//从堆栈上弹出value，并将其设到全局变量key中

	UNLOCK(E)
}

void
skynet_env_init() {
	E = skynet_malloc(sizeof(*E));//分配环境内存
	E->lock = 0;//初始化锁
	E->L = luaL_newstate();//创建lua虚拟机
}
