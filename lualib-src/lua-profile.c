//统计一个消息处理使用的系统时间
#include <lua.h>
#include <lauxlib.h>

#include <time.h>

#if defined(__APPLE__)
#include <mach/task.h>
#include <mach/mach.h>
#endif

#define NANOSEC 1000000000 //纳秒
#define MICROSEC 1000000 //微妙

static double
get_time() {
#if  !defined(__APPLE__) //如果非apple平台
	// 	struct timespec
	// {
	//     time_t tv_sec;  秒     
	//     long int tv_nsec; 纳秒     
	// };
	struct timespec ti;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti);//本线程到当前代码系统CPU花费的时间

	int sec = ti.tv_sec & 0xffff;//秒只取32位
	int nsec = ti.tv_nsec;//纳秒

	return (double)sec + (double)nsec / NANOSEC;//将纳秒转化为秒
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
diff_time(double start) {//计算时间的差值
	double now = get_time();//当前时间
	if (now < start) {
		return now + 0x10000 - start;//??
	} else {
		return now - start;
	}
}

//当前协程计时开始
//初始化启动时间(start time)为当前时间
//初始化全部时间(total time)为0
static int
lstart(lua_State *L) {
	lua_pushthread(L);//将当前线程co压入栈
	lua_rawget(L, lua_upvalueindex(2));//获取表B[co]的值
	if (!lua_isnil(L, -1)) {//如果值不为空
		//线程只能启动profile一次
		return luaL_error(L, "Thread %p start profile more than once", lua_topointer(L, 1));
	}
	lua_pushthread(L);//压入co
	lua_pushnumber(L, 0);//压入0
	lua_rawset(L, lua_upvalueindex(2));//表B[co]=0

	lua_pushthread(L);//压入co
	lua_pushnumber(L, get_time());//压入启动时间
	lua_rawset(L, lua_upvalueindex(1));//表A[co]=启动时间

	return 0;
}

//当前协程停止计时
//计算当前时间距离启动时间(启动时间会挂载恢复的时候会重置)的差值，加上全部的时间，即为消耗的总时间
static int
lstop(lua_State *L) {
	lua_pushthread(L);//压入co
	lua_rawget(L, lua_upvalueindex(1));//获取表A[co]的值
	luaL_checktype(L, -1, LUA_TNUMBER);//检查类型是否为number
	double ti = diff_time(lua_tonumber(L, -1));//计算时间差值
	lua_pushthread(L);//压入co
	lua_rawget(L, lua_upvalueindex(2));//获取表B[co]的值
	double total_time = lua_tonumber(L, -1);//总时间

	//表A[co]=nil
	lua_pushthread(L);
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(1));

	//表B[co]=nil
	lua_pushthread(L);
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(2));

	//压入返回结果
	lua_pushnumber(L, ti + total_time);

	return 1;
}

//恢复协程
//恢复协程运行需要重置启动时间
static int
lresume(lua_State *L) {
	lua_pushvalue(L,1);//复制co参数并压栈，resume第一个参数为co
	lua_rawget(L, lua_upvalueindex(2));//获取表B[co]的值，然后将值压入

	//check total time为nil，则代表该协程没有使用profile记录时间
	if (lua_isnil(L, -1)) {		// check total time 检查总时间，为NILL则表示没有找到值
		lua_pop(L,1);//弹出空值
	} else {
		lua_pop(L,1);//弹出取到的总时间
		lua_pushvalue(L,1);//复制co参数并压栈
		double ti = get_time();//获取当前时间
		lua_pushnumber(L, ti);//将时间压入堆栈
		lua_rawset(L, lua_upvalueindex(1));	// set start time 重置启动时间，表A[co]=ti
	}

	lua_CFunction co_resume = lua_tocfunction(L, lua_upvalueindex(3));//获取LUA中的resume

	return co_resume(L);//调用coroutine.resume
}

//挂起协程
//挂起协程时，需要计算当前走过的时间，然后加到total time内
static int
lyield(lua_State *L) {
	lua_pushthread(L);//将当前线程(co)压栈
	lua_rawget(L, lua_upvalueindex(2));	// check total time 获取表B[co]的值，压栈
	if (lua_isnil(L, -1)) {//获取到的值为空
		lua_pop(L,1);//pop出空值
	} else {
		double ti = lua_tonumber(L, -1);//将栈上的时间转化为整数
		lua_pop(L,1);//弹出

		lua_pushthread(L);//将当前线程(co)压栈
		lua_rawget(L, lua_upvalueindex(1));//获取表A[co]的值，压栈
		double starttime = lua_tonumber(L, -1);//将栈上的时间转化为整数
		lua_pop(L,1);//弹出

		ti += diff_time(starttime);//计算经历的时间

		lua_pushthread(L);//压入co
		lua_pushnumber(L, ti);//压入经历的时间
		lua_rawset(L, lua_upvalueindex(2));//表B[co]=ti
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
	lua_newtable(L);	// table thread->start time 假设名为表A，保存启动时间的表
	lua_newtable(L);	// table thread->total time 假设名为表B，保存全部时间的表

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
