//核心库， 封装 skynet 给 lua 使用
#include "skynet.h"
#include "lua-seri.h"

#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"

#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

struct snlua {
	lua_State * L;
	struct skynet_context * ctx;
	const char * preload;
};

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

static int
_cb(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	lua_State *L = ud;//从用户数据中获取lua_State
	int trace = 1;
	int r;
	int top = lua_gettop(L);//获取栈顶索引
	//printf("_cb:the top of stack is %d \n",lua_gettop(L));	

	if (top == 0) {//栈为空
		lua_pushcfunction(L, traceback);//压入追踪函数
		lua_rawgetp(L, LUA_REGISTRYINDEX, _cb);//根据cb获取LUA回调函数(_callback传入的)
	} else {
		assert(top == 2);//栈内有两个元素
	}
	lua_pushvalue(L,2);//复制LUA回调函数

	lua_pushinteger(L, type);//压入类型类型
	lua_pushlightuserdata(L, (void *)msg);//压入消息
	lua_pushinteger(L,sz);//压入消息大小
	lua_pushinteger(L, session);//压入会话
	lua_pushnumber(L, source);//压入消息来源

	r = lua_pcall(L, 5, 0 , trace);//调用回调函数处理消息

	if (r == LUA_OK) {//成功
		return 0;//直接返回0
	}
	const char * self = skynet_command(context, "REG", NULL);//获取自己句柄的十六进制表示(字符串)
	switch (r) {
	case LUA_ERRRUN://运行时错误
		skynet_error(context, "lua call [%x to %s : %d msgsz = %d] error : " KRED "%s" KNRM, source , self, session, sz, lua_tostring(L,-1));
		break;
	case LUA_ERRMEM://内存分配错误
		skynet_error(context, "lua memory error : [%x to %s : %d]", source , self, session);
		break;
	case LUA_ERRERR://在运行错误处理函数时发生的错误
		skynet_error(context, "lua error in error : [%x to %s : %d]", source , self, session);
		break;
	case LUA_ERRGCMM://在运行 __gc 元方法时发生的错误
		skynet_error(context, "lua gc error : [%x to %s : %d]", source , self, session);
		break;
	};

	lua_pop(L,1);//弹出复制的LUA回调函数

	return 0;
}

static int
forward_cb(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	_cb(context, ud, type, session, source, msg, sz);
	// don't delete msg in forward mode.
	return 1;
}

static int
_callback(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));//从上值取得上下文
	int forward = lua_toboolean(L, 2);
	luaL_checktype(L,1,LUA_TFUNCTION);//检查参数1是否是函数
	lua_settop(L,1);//设置栈顶
	lua_rawsetp(L, LUA_REGISTRYINDEX, _cb);//注册表[_cb对应的轻量用户数据]=栈顶的LUA函数,会将值弹出栈

	lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);//获取LUA状态机的主线程(类型可能是thread吧)，压入
	lua_State *gL = lua_tothread(L,-1);//将栈顶的值转化为LUA线程，由lua_State*表示

	//printf("_callback:the top of stack is %d \n",lua_gettop(L));	
	if (forward) {
		skynet_callback(context, gL, forward_cb);
	} else {
		skynet_callback(context, gL, _cb);//重新设置上下文的回调函数
	}

	return 0;
}

static int
_command(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	const char * cmd = luaL_checkstring(L,1);
	const char * result;
	const char * parm = NULL;
	if (lua_gettop(L) == 2) {
		parm = luaL_checkstring(L,2);
	}

	result = skynet_command(context, cmd, parm);
	if (result) {
		lua_pushstring(L, result);
		return 1;
	}
	return 0;
}

static int
_genid(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	int session = skynet_send(context, 0, 0, PTYPE_TAG_ALLOCSESSION , 0 , NULL, 0);
	lua_pushinteger(L, session);
	return 1;
}

//获取目标字符串地址
static const char *
get_dest_string(lua_State *L, int index) {
	const char * dest_string = lua_tostring(L, index);//从栈的指定索引处获取字符串
	if (dest_string == NULL) {//获取到的字符串为空
		//目标地址一定为数字或者是字符串
		luaL_error(L, "dest address type (%s) must be a string or number.", lua_typename(L, lua_type(L,index)));
	}
	return dest_string;
}

/*
	参数：
		1：unsigned address或者string，address 数字地址或者字符串地址
		2. integer type，消息类型id
		3. integer session，会话
		4. string message或者lightuserdata message_ptr，字符串消息或者是轻用户数据消息
		5. integer len
 */
static int
_send(lua_State *L) {
	//lua_upvalueindex获取到当前运行的函数的第i个上值的伪索引
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));//从伪索引获取到上下文
	uint32_t dest = lua_tounsigned(L, 1);//获取目标数字地址
	const char * dest_string = NULL;//目标字符串地址
	if (dest == 0) {//如果目标数字地址为0 传入的不是数字 而是字符串
		dest_string = get_dest_string(L, 1);//获取目标字符串地址
	}

	int type = luaL_checkinteger(L, 2);//获取消息类型
	int session = 0;
	if (lua_isnil(L,3)) {//如果第三个参数是空的话，则是skynet.call调用过来的
		type |= PTYPE_TAG_ALLOCSESSION;//skynet.call会在内部生成一个唯一session，所以这里需要设置分配会话标志
	} else {
		session = luaL_checkinteger(L,3);//获取会话
		//skynet.send调用过来的传递的是0
	}

	int mtype = lua_type(L,4);//取得第四个参数的类型
	switch (mtype) {//判断类型
	case LUA_TSTRING: {//如果是字符串
		size_t len = 0; //存放字符串长度
		void * msg = (void *)lua_tolstring(L,4,&len);//获取字符串
		if (len == 0) {//长度为零
			msg = NULL;//置空字符串指针
		}
		if (dest_string) {//目标地址是字符串地址
			session = skynet_sendname(context, 0, dest_string, type, session , msg, len);//需要拷贝消息数据
		} else {//目标地址是数字地址
			session = skynet_send(context, 0, dest, type, session , msg, len);//需要拷贝消息数据
		}
		break;
	}
	case LUA_TLIGHTUSERDATA: {//如果是轻用户数据
		void * msg = lua_touserdata(L,4);//获取消息的C指针
		int size = luaL_checkinteger(L,5);//获取消息的长度
		if (dest_string) {//如果目标是字符串地址
			//不需要拷贝消息数据
			session = skynet_sendname(context, 0, dest_string, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		} else {//如果目标是数字地址
			//不需要拷贝消息数据
			session = skynet_send(context, 0, dest, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		}
		break;
	}
	default:
		luaL_error(L, "skynet.send invalid param %s", lua_typename(L, lua_type(L,4)));
	}
	if (session < 0) {
		// send to invalid address
		// todo: maybe throw error whould be better
		// 发送到无效的地址
		// 可能抛出一个错误更好
		return 0;
	}
	lua_pushinteger(L,session);//返回会话
	return 1;
}

static int
_redirect(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	uint32_t dest = lua_tounsigned(L,1);
	const char * dest_string = NULL;
	if (dest == 0) {
		dest_string = get_dest_string(L, 1);
	}
	uint32_t source = luaL_checkunsigned(L,2);
	int type = luaL_checkinteger(L,3);
	int session = luaL_checkinteger(L,4);

	int mtype = lua_type(L,5);
	switch (mtype) {
	case LUA_TSTRING: {
		size_t len = 0;
		void * msg = (void *)lua_tolstring(L,5,&len);
		if (len == 0) {
			msg = NULL;
		}
		if (dest_string) {
			session = skynet_sendname(context, source, dest_string, type, session , msg, len);
		} else {
			session = skynet_send(context, source, dest, type, session , msg, len);
		}
		break;
	}
	case LUA_TLIGHTUSERDATA: {
		void * msg = lua_touserdata(L,5);
		int size = luaL_checkinteger(L,6);
		if (dest_string) {
			session = skynet_sendname(context, source, dest_string, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		} else {
			session = skynet_send(context, source, dest, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		}
		break;
	}
	default:
		luaL_error(L, "skynet.redirect invalid param %s", lua_typename(L,mtype));
	}
	return 0;
}

static int
_error(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	skynet_error(context, "%s", luaL_checkstring(L,1));
	return 0;
}

static int
_tostring(lua_State *L) {
	if (lua_isnoneornil(L,1)) {
		return 0;
	}
	char * msg = lua_touserdata(L,1);
	int sz = luaL_checkinteger(L,2);
	lua_pushlstring(L,msg,sz);
	return 1;
}

static int
_harbor(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	uint32_t handle = luaL_checkunsigned(L,1);
	int harbor = 0;
	int remote = skynet_isremote(context, handle, &harbor);
	lua_pushinteger(L,harbor);
	lua_pushboolean(L, remote);

	return 2;
}

static int
lpackstring(lua_State *L) {
	_luaseri_pack(L);
	char * str = (char *)lua_touserdata(L, -2);
	int sz = lua_tointeger(L, -1);
	lua_pushlstring(L, str, sz);
	skynet_free(str);
	return 1;
}

static int
ltrash(lua_State *L) {
	int t = lua_type(L,1);
	switch (t) {
	case LUA_TSTRING: {
		break;
	}
	case LUA_TLIGHTUSERDATA: {
		void * msg = lua_touserdata(L,1);
		luaL_checkinteger(L,2);
		skynet_free(msg);
		break;
	}
	default:
		luaL_error(L, "skynet.trash invalid param %s", lua_typename(L,t));
	}

	return 0;
}

int
luaopen_skynet_core(lua_State *L) {
	luaL_checkversion(L);//检查版本
	
	luaL_Reg l[] = {//函数名->函数 映射表
		{ "send" , _send }, //发送消息函数
		{ "genid", _genid },
		{ "redirect", _redirect },
		{ "command" , _command },
		{ "error", _error },
		{ "tostring", _tostring },
		{ "harbor", _harbor },
		{ "pack", _luaseri_pack },//skynet.pack调用该函数
		{ "unpack", _luaseri_unpack },//skynet.unpack调用该函数
		{ "packstring", lpackstring },
		{ "trash" , ltrash },
		{ "callback", _callback },
		{ NULL, NULL },
	};
	
	luaL_newlibtable(L, l);//创建一张新的表，并预分配足够保存下数组 l 内容的空间（但不填充）
	//而luaL_newlib() 是创建一张新的表，并把列表 l 中的函数注册进去。

	lua_getfield(L, LUA_REGISTRYINDEX, "skynet_context");//获取skynet_context的值并压栈
	struct skynet_context *ctx = lua_touserdata(L,-1);//将light userdata转化成C指针 检查是否存在
	if (ctx == NULL) {
		return luaL_error(L, "Init skynet context first");//提示先初始化skynet context
	}
	//skynet_context将作为上值压在新建的表上面

	luaL_setfuncs(L,l,1);//注册所有函数到栈顶的表（如果存在上值，那么上值将压在该表的上面，所有函数共享这些上值，第三个参数表明有几个上值，这些值必须在调用该函数前，压在表上面，注册完毕后，都会从栈上弹出）

	return 1;
}
