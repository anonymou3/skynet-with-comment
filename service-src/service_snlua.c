//加载 lua 编写的服务
#include "skynet.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

//snlua数据结构定义
struct snlua {
	lua_State * L;//一个lua虚拟机
	struct skynet_context * ctx;//skynet上下文
};

// LUA_CACHELIB may defined in patched lua for shared proto
// LUA_CACHELIB	可能定义在打过补丁的lua中用于共享proto
#ifdef LUA_CACHELIB

#define codecache luaopen_cache	//使用lua中的函数

#else	//使用自定义的函数

//lua_CFunction的原型：typedef int (*lua_CFunction) (lua_State *L);

//C 函数的类型。

// 为了正确的和 Lua 通讯， C 函数必须使用下列协议。 这个协议定义了参数以及返回值传递方法： C 函数通过 Lua 中的栈来接受参数， 参数以正序入栈（第一个参数首先入栈）。 因此，当函数开始的时候， lua_gettop(L) 可以返回函数收到的参数个数。 第一个参数（如果有的话）在索引 1 的地方， 而最后一个参数在索引 lua_gettop(L) 处。 当需要向 Lua 返回值的时候， C 函数只需要把它们以正序压到堆栈上（第一个返回值最先压入）， 然后返回这些返回值的个数。 在这些返回值之下的，堆栈上的东西都会被 Lua 丢掉。 和 Lua 函数一样，从 Lua 中调用 C 函数也可以有很多返回值。

//也就是说，在C中定义的函数如果想作为C库被LUA调用，C函数内使用的参数是不需要通过声明的，而是在代码里操作LUA栈来获得
//而在LUA这面，像调用普通函数那样传入参数即可，两者是通过约定协议来交互的。


//需要注意的是：无论何时 Lua 调用 C，被调用的函数都得到一个新的栈， 这个栈独立于 C 函数本身的栈，也独立于之前的 Lua 栈。它里面包含了 Lua 传递给 C 函数的所有参数， 而 C 函数则把要返回的结果放入这个栈以返回给调用者

static int
cleardummy(lua_State *L) {
  return 0;
}

static int 
codecache(lua_State *L) {
	luaL_Reg l[] = {
		{ "clear", cleardummy },
		{ NULL, NULL },
	};
	luaL_newlib(L,l);
	lua_getglobal(L, "loadfile");
	lua_setfield(L, -2, "loadfile");
	return 1;
}

#endif

static int 
traceback (lua_State *L) {
	const char *msg = lua_tostring(L, 1);
	if (msg)
		luaL_traceback(L, L, msg, 1);
	else {
		lua_pushliteral(L, "(no error message)");
	}
	return 1;
}

static void
_report_launcher_error(struct skynet_context *ctx) {
	// sizeof "ERROR" == 5
	skynet_sendname(ctx, 0, ".launcher", PTYPE_TEXT, 0, "ERROR", 5);
}

static const char *
optstring(struct skynet_context *ctx, const char *key, const char * str) {
	const char * ret = skynet_command(ctx, "GETENV", key);
	if (ret == NULL) {
		return str;
	}
	return ret;
}

static int
_init(struct snlua *l, struct skynet_context *ctx, const char * args, size_t sz) {
	lua_State *L = l->L;//从snlua中获取lua虚拟机
	l->ctx = ctx;//设置snlua中的上下文指针


	// 	使用自动 gc 会有一个问题。它很可能使系统的峰值内存占用远超过实际需求量。原因就在于，收集行为往往发生在调用栈很深的地方。当你的应用程序呈现出某种周期性（大多数包驱动的服务都是这样）。在一个服务周期内，往往会引用众多临时对象，这个时候做 mark 工作，会导致许多临时对象也被 mark 住。
	// 一个经验方法是，调用LUA_GCSTOP停止自动 GC。在周期间定期调用 gcstep 且使用较大的 data 值，在有限个周期做完一整趟 gc 。
	lua_gc(L, LUA_GCSTOP, 0);//停止垃圾收集器


	lua_pushboolean(L, 1);  /* signal for libraries to ignore env. vars. */	//向栈中压入一个布尔true
	//Lua 提供了一个注册表，这是一个预定义出来的表，可以用来保存任何 C 代码想保存的 Lua 值。
	//LUA_REGISTRYINDEX是伪索引，用来索引注册表
	lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");//注册表["LUA_NOENV"]=true
	luaL_openlibs(L);//打开所有Lua标准库
	lua_pushlightuserdata(L, ctx);//将skynet_context指针作为light userdata压栈
	lua_setfield(L, LUA_REGISTRYINDEX, "skynet_context");//注册表["skynet_context"]=light userdata
	luaL_requiref(L, "skynet.codecache", codecache , 0);//require进函数
	lua_pop(L,1);

	const char *path = optstring(ctx, "lua_path","./lualib/?.lua;./lualib/?/init.lua");//获取lua path
	lua_pushstring(L, path);
	lua_setglobal(L, "LUA_PATH");
	const char *cpath = optstring(ctx, "lua_cpath","./luaclib/?.so");
	lua_pushstring(L, cpath);
	lua_setglobal(L, "LUA_CPATH");
	const char *service = optstring(ctx, "luaservice", "./service/?.lua");
	lua_pushstring(L, service);
	lua_setglobal(L, "LUA_SERVICE");
	const char *preload = skynet_command(ctx, "GETENV", "preload");
	lua_pushstring(L, preload);
	lua_setglobal(L, "LUA_PRELOAD");

	lua_pushcfunction(L, traceback);
	assert(lua_gettop(L) == 1);

	const char * loader = optstring(ctx, "lualoader", "./lualib/loader.lua");

	int r = luaL_loadfile(L,loader);
	if (r != LUA_OK) {
		skynet_error(ctx, "Can't load %s : %s", loader, lua_tostring(L, -1));
		_report_launcher_error(ctx);
		return 1;
	}
	lua_pushlstring(L, args, sz);
	r = lua_pcall(L,1,0,1);
	if (r != LUA_OK) {
		skynet_error(ctx, "lua loader error : %s", lua_tostring(L, -1));
		_report_launcher_error(ctx);
		return 1;
	}
	lua_settop(L,0);

	lua_gc(L, LUA_GCRESTART, 0);//重启垃圾收集器

	return 0;
}

//上下文回调函数（启动函数）
static int
_launch(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz) {
	assert(type == 0 && session == 0);//断言类型和会话都等于0
	struct snlua *l = ud;//从用户数据中获取到snlua引用
	skynet_callback(context, NULL, NULL);//将上下文的回调函数和用户数据清空
	int err = _init(l, context, msg, sz);//调用初始化函数
	if (err) {//如果有错误
		skynet_command(context, "EXIT", NULL);//执行skynet的退出命令
	}

	return 0;
}

int
snlua_init(struct snlua *l, struct skynet_context *ctx, const char * args) {
	int sz = strlen(args);//参数长度
	char * tmp = skynet_malloc(sz);//分配内存存储参数
	memcpy(tmp, args, sz);//拷贝参数数据 不包含"\0"
	skynet_callback(ctx, l , _launch);//设置上下文回调函数
	const char * self = skynet_command(ctx, "REG", NULL);//self为服务句柄号十六进制表示

	//str to unsigned long  将字符串转换成无符号长整型数
	uint32_t handle_id = strtoul(self+1, NULL, 16);//计算出该snlua服务的句柄号
	// it must be first message
	// 发送第一个消息 其实第一个消息就是"bootstrap"
	skynet_send(ctx, 0, handle_id, PTYPE_TAG_DONTCOPY,0, tmp, sz);//源地址为0，目的地址是自己，自己给自己发送一个消息
	return 0;
}

struct snlua *
snlua_create(void) {
	struct snlua * l = skynet_malloc(sizeof(*l));//先为snlua数据结构分配内存
	memset(l,0,sizeof(*l));//清空内存
	l->L = lua_newstate(skynet_lalloc, NULL);//创建一个虚拟机
	return l;//返回snlua引用
}

void
snlua_release(struct snlua *l) {
	lua_close(l->L);//关闭虚拟机
	skynet_free(l);//释放snlua内存
}
