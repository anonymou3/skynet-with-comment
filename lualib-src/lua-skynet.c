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
	lua_State *L = ud;
	int trace = 1;
	int r;
	int top = lua_gettop(L);
	if (top == 0) {
		lua_pushcfunction(L, traceback);
		lua_rawgetp(L, LUA_REGISTRYINDEX, _cb);
	} else {
		assert(top == 2);
	}
	lua_pushvalue(L,2);

	lua_pushinteger(L, type);
	lua_pushlightuserdata(L, (void *)msg);
	lua_pushinteger(L,sz);
	lua_pushinteger(L, session);
	lua_pushnumber(L, source);

	r = lua_pcall(L, 5, 0 , trace);

	if (r == LUA_OK) {
		return 0;
	}
	const char * self = skynet_command(context, "REG", NULL);
	switch (r) {
	case LUA_ERRRUN:
		skynet_error(context, "lua call [%x to %s : %d msgsz = %d] error : " KRED "%s" KNRM, source , self, session, sz, lua_tostring(L,-1));
		break;
	case LUA_ERRMEM:
		skynet_error(context, "lua memory error : [%x to %s : %d]", source , self, session);
		break;
	case LUA_ERRERR:
		skynet_error(context, "lua error in error : [%x to %s : %d]", source , self, session);
		break;
	case LUA_ERRGCMM:
		skynet_error(context, "lua gc error : [%x to %s : %d]", source , self, session);
		break;
	};

	lua_pop(L,1);

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
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	int forward = lua_toboolean(L, 2);
	luaL_checktype(L,1,LUA_TFUNCTION);
	lua_settop(L,1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, _cb);

	lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
	lua_State *gL = lua_tothread(L,-1);

	if (forward) {
		skynet_callback(context, gL, forward_cb);
	} else {
		skynet_callback(context, gL, _cb);
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

//获取目标地址字符串
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
	unsigned address
	 string address
	integer type
	integer session
	string message
	 lightuserdata message_ptr
	 integer len
 */
static int
_send(lua_State *L) {
	//lua_upvalueindex获取到当前运行的函数的第i个上值的伪索引
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));//从伪索引获取到上下文
	uint32_t dest = lua_tounsigned(L, 1);//获取目标地址
	const char * dest_string = NULL;//目标地址字符串
	if (dest == 0) {//如果目标地址为0 传入的不是数字
		dest_string = get_dest_string(L, 1);
	}

	int type = luaL_checkinteger(L, 2);
	int session = 0;
	if (lua_isnil(L,3)) {
		type |= PTYPE_TAG_ALLOCSESSION;
	} else {
		session = luaL_checkinteger(L,3);
	}

	int mtype = lua_type(L,4);
	switch (mtype) {
	case LUA_TSTRING: {
		size_t len = 0;
		void * msg = (void *)lua_tolstring(L,4,&len);
		if (len == 0) {
			msg = NULL;
		}
		if (dest_string) {
			session = skynet_sendname(context, 0, dest_string, type, session , msg, len);
		} else {
			session = skynet_send(context, 0, dest, type, session , msg, len);
		}
		break;
	}
	case LUA_TLIGHTUSERDATA: {
		void * msg = lua_touserdata(L,4);
		int size = luaL_checkinteger(L,5);
		if (dest_string) {
			session = skynet_sendname(context, 0, dest_string, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		} else {
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
		return 0;
	}
	lua_pushinteger(L,session);
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
		{ "send" , _send },
		{ "genid", _genid },
		{ "redirect", _redirect },
		{ "command" , _command },
		{ "error", _error },
		{ "tostring", _tostring },
		{ "harbor", _harbor },
		{ "pack", _luaseri_pack },
		{ "unpack", _luaseri_unpack },
		{ "packstring", lpackstring },
		{ "trash" , ltrash },
		{ "callback", _callback },
		{ NULL, NULL },
	};
	//luaL_newlib() 创建一张新的表，并把列表 l 中的函数注册进去。
	luaL_newlibtable(L, l);//创建一张新的表，并预分配足够保存下数组 l 内容的空间（但不填充）

	lua_getfield(L, LUA_REGISTRYINDEX, "skynet_context");//获取skynet_context的值并压栈
	struct skynet_context *ctx = lua_touserdata(L,-1);//将light userdata转化成C指针 检查是否存在
	if (ctx == NULL) {
		return luaL_error(L, "Init skynet context first");//提示先初始化skynet context
	}
	//skynet_context将作为上值压在新建的表上面

	luaL_setfuncs(L,l,1);//注册所有函数到栈顶的表（如果存在上值，那么上值将压在该表的上面，所有函数共享这些上值，第三个参数表明有几个上值，这些值必须在调用该函数前，压在表上面，注册完毕后，都会从栈上弹出）

	return 1;
}
