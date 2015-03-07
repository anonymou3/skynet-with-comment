//统计一个消息处理使用的系统时间
#include <lua.h>
#include <lauxlib.h>

#include <time.h>

#if defined(__APPLE__)
#include <mach/task.h>
#include <mach/mach.h>
#endif

#define NANOSEC 1000000000
#define MICROSEC 1000000

static double
get_time() {
#if  !defined(__APPLE__)
	struct timespec ti;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti);

	int sec = ti.tv_sec & 0xffff;
	int nsec = ti.tv_nsec;

	return (double)sec + (double)nsec / NANOSEC;	
#else
	struct task_thread_times_info aTaskInfo;
	mach_msg_type_number_t aTaskInfoCount = TASK_THREAD_TIMES_INFO_COUNT;
	if (KERN_SUCCESS != task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, (task_info_t )&aTaskInfo, &aTaskInfoCount)) {
		return 0;
	}

	int sec = aTaskInfo.user_time.seconds & 0xffff;
	int msec = aTaskInfo.user_time.microseconds;

	return (double)sec + (double)msec / MICROSEC;
#endif
}

static inline double 
diff_time(double start) {
	double now = get_time();
	if (now < start) {
		return now + 0x10000 - start;
	} else {
		return now - start;
	}
}

static int
lstart(lua_State *L) {
	lua_pushthread(L);
	lua_rawget(L, lua_upvalueindex(2));
	if (!lua_isnil(L, -1)) {
		return luaL_error(L, "Thread %p start profile more than once", lua_topointer(L, 1));
	}
	lua_pushthread(L);
	lua_pushnumber(L, 0);
	lua_rawset(L, lua_upvalueindex(2));

	lua_pushthread(L);
	lua_pushnumber(L, get_time());
	lua_rawset(L, lua_upvalueindex(1));

	return 0;
}

static int
lstop(lua_State *L) {
	lua_pushthread(L);
	lua_rawget(L, lua_upvalueindex(1));
	luaL_checktype(L, -1, LUA_TNUMBER);
	double ti = diff_time(lua_tonumber(L, -1));
	lua_pushthread(L);
	lua_rawget(L, lua_upvalueindex(2));
	double total_time = lua_tonumber(L, -1);

	lua_pushthread(L);
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(1));

	lua_pushthread(L);
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(2));

	lua_pushnumber(L, ti + total_time);

	return 1;
}

static int
lresume(lua_State *L) {
	lua_pushvalue(L,1);//复制co参数并压栈
	lua_rawget(L, lua_upvalueindex(2));//获取表B[co]的值，然后将值压入
	if (lua_isnil(L, -1)) {		// check total time 检查总时间 没有找到值
		lua_pop(L,1);//弹出空值
	} else {
		lua_pop(L,1);
		lua_pushvalue(L,1);
		double ti = get_time();
		lua_pushnumber(L, ti);
		lua_rawset(L, lua_upvalueindex(1));	// set start time
	}

	lua_CFunction co_resume = lua_tocfunction(L, lua_upvalueindex(3));

	return co_resume(L);
}

static int
lyield(lua_State *L) {
	lua_pushthread(L);
	lua_rawget(L, lua_upvalueindex(2));	// check total time
	if (lua_isnil(L, -1)) {
		lua_pop(L,1);
	} else {
		double ti = lua_tonumber(L, -1);
		lua_pop(L,1);

		lua_pushthread(L);
		lua_rawget(L, lua_upvalueindex(1));
		double starttime = lua_tonumber(L, -1);
		lua_pop(L,1);

		ti += diff_time(starttime);

		lua_pushthread(L);
		lua_pushnumber(L, ti);
		lua_rawset(L, lua_upvalueindex(2));
	}

	lua_CFunction co_yield = lua_tocfunction(L, lua_upvalueindex(3));

	return co_yield(L);
}

int
luaopen_profile(lua_State *L) {
	luaL_checkversion(L); //检查版本
	luaL_Reg l[] = {//映射表
		{ "start", lstart },
		{ "stop", lstop },
		{ "resume", lresume },
		{ "yield", lyield },
		{ NULL, NULL },
	};
	luaL_newlibtable(L,l);//为存储映射表新建一个表
	lua_newtable(L);	// table thread->start time 假设名为表A
	lua_newtable(L);	// table thread->total time 假设名为表B

	lua_newtable(L);	// weak table 弱表,实际准确说是**弱表的元表**
	lua_pushliteral(L, "kv");//push字面量"kv"
	lua_setfield(L, -2, "__mode");//weaktable["__mode"]="kv",表示key和value都是weak的，能够被自动gc

	lua_pushvalue(L, -1);//复制刚才的**弱表的元表**然后压栈
	lua_setmetatable(L, -3); //为表B设置元表，表B成为弱表
	lua_setmetatable(L, -3);//为表A设置元表，表A成为弱表

	lua_pushnil(L);//压一个空值
	luaL_setfuncs(L,l,3);//注册映射表中的所有函数，并且有3个上值，所以注册的所有函数前两个参数是表A，表B，第三个参数是空

	int libtable = lua_gettop(L);//获取栈顶元素的索引

	lua_getglobal(L, "coroutine");//获取coroutine，实际上是一个table
	lua_getfield(L, -1, "resume");//获取coroutine["resume"]并压栈,实际上是一个function，也就是平常调用的coroutine.resume

	lua_CFunction co_resume = lua_tocfunction(L, -1);//将coroutine.resume转化为C函数，可以这么转换么？
	if (co_resume == NULL)
		return luaL_error(L, "Can't get coroutine.resume");
	lua_pop(L,1);//将coroutine.resume弹出
	lua_getfield(L, libtable, "resume");//获取新建表中的resume函数(C函数)
	lua_pushcfunction(L, co_resume);//将LUA中的resume压栈
	lua_setupvalue(L, -2, 3);//弹出lua中的resume,设置C的resume函数的第三个上值为lua中的resume
	lua_pop(L,1);//弹出C的resume

	lua_getfield(L, -1, "yield");//获取coroutine["yield"]

	lua_CFunction co_yield = lua_tocfunction(L, -1);//转化为C函数
	if (co_yield == NULL)
		return luaL_error(L, "Can't get coroutine.yield");
	lua_pop(L,1);//弹出cotoutine.yield
	lua_getfield(L, libtable, "yield");//获取C的yield
	lua_pushcfunction(L, co_yield);//压入lua的yield
	lua_setupvalue(L, -2, 3);//弹出lua的yield,并设置为C的yield的第三个上值

	lua_settop(L, libtable);//设置栈顶
	return 1;
}
