#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_env.h"
#include "skynet_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <signal.h>
#include <assert.h>

static int
optint(const char *key, int opt) {
	const char * str = skynet_getenv(key);//从环境中获取key对应的value
	if (str == NULL) {//如果为空的话	
		//将opt的值存入到环境中
		char tmp[20];
		sprintf(tmp,"%d",opt);
		skynet_setenv(key, tmp);
		return opt;
	}
	return strtol(str, NULL, 10);//将字符串转换成整数
}

/*
static int
optboolean(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		skynet_setenv(key, opt ? "true" : "false");
		return opt;
	}
	return strcmp(str,"true")==0;
}
*/

static const char *
optstring(const char *key,const char * opt) {//作用同optint,区别是这里取字符串
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		if (opt) {
			skynet_setenv(key, opt);
			opt = skynet_getenv(key);
		}
		return opt;
	}
	return str;
}

// lua_next() 这个函数的工作过程是：
// 1) 先从栈顶弹出一个 key(所以在遍历table的时候先压入一个空值，key为空代表取第一对key-value)
// 2) 从栈指定位置的 table 里取下一对 key-value，先将 key 入栈再将 value 入栈
// 3) 如果第 2 步成功则返回非 0 值，否则返回 0，并且不向栈中压入任何值
 
static void
_init_env(lua_State *L) {
	lua_pushnil(L);  /* first key */	//压入一个空值
	while (lua_next(L, -2) != 0) {//遍历table					
		int keyt = lua_type(L, -2);//检查key的类型
		if (keyt != LUA_TSTRING) {//key的类型不是字符串
			fprintf(stderr, "Invalid config table\n");//报错
			exit(1);//退出程序
		}
		const char * key = lua_tostring(L,-2);//取得key
		if (lua_type(L,-1) == LUA_TBOOLEAN) {//如果值是BOOL
			int b = lua_toboolean(L,-1);//取得值
			skynet_setenv(key,b ? "true" : "false" );//设置到环境中
		} else {
			const char * value = lua_tostring(L,-1);//取得值
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			skynet_setenv(key,value);//设置到环境中
		}
		lua_pop(L,1);//弹出value,继续遍历
	}
	lua_pop(L,1);//弹出table(load_config中的result)
}

int sigign() {
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, 0);//重载信号处理方法，屏蔽SIGPIPE,防止进程退出
	return 0;
}

//函数作用：
//1.获取配置文件名
//2.打开文件
//3.读取文件
//4.os.getenv(name)是从系统环境变量中获取name的值，如果没定义则返回nil，而当assert的第一个参数为nil时，将返回name
//5.[%w_%d]创建用户自己的字符分类（字符集），同时匹配字母、数字和下划线。模式匹配圆括号有什么特殊意义？（圆括号是LUA里的捕获功能）
//第五步的大概意思就是匹配配置文件中类似$PATH的字符串，并从系统环境变量取得值然后替换之，如果没有，维持不变
//6.关闭文件
//7.定义一个result表
//8.从字符串代码块中加载代码，将结果存入result表中
//9.返回result表
static const char * load_config = "\
	local config_name = ...\
	local f = assert(io.open(config_name))\
	local code = assert(f:read \'*a\')\
	local function getenv(name) return assert(os.getenv(name), name) end\
	code = string.gsub(code, \'%$([%w_%d]+)\', getenv)\
	f:close()\
	local result = {}\
	assert(load(code,\'=(load)\',\'t\',result))()\
	return result\
";
//入口函数
int
main(int argc, char *argv[]) {
	const char * config_file = NULL ;//存放配置文件名
	if (argc > 1) {//argc包括可执行文件名和后续的参数
		config_file = argv[1];//取得配置文件名
	} else {
		fprintf(stderr, "Need a config file. Please read skynet wiki : https://github.com/cloudwu/skynet/wiki/Config\n"
			"usage: skynet configfilename\n");
		return 1;
	}
	skynet_globalinit();//全局初始化
	skynet_env_init();//环境初始化

	sigign();//SIGPIPE信号处理

	struct skynet_config config;//skynet配置

	struct lua_State *L = lua_newstate(skynet_lalloc, NULL);//创建lua虚拟机，并提供内存分配器函数（为何提供还不知道),该虚拟机用于解析配置文件
	luaL_openlibs(L);	// link lua lib（打开标准库）

	int err = luaL_loadstring(L, load_config);//将解析配置文件功能的代码作为函数压栈
	assert(err == LUA_OK);
	lua_pushstring(L, config_file);//压入配置文件名
	
	err = lua_pcall(L, 1, 1, 0);//调用函数解析配置文件
	if (err) {
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		lua_close(L);
		return 1;
	} 
	_init_env(L);//将配置存入环境(其实是存到了lua虚拟机)

	//从环境读取配置
	config.thread =  optint("thread",8);
	config.module_path = optstring("cpath","./cservice/?.so");
	config.harbor = optint("harbor", 1);
	config.bootstrap = optstring("bootstrap","snlua bootstrap");
	config.daemon = optstring("daemon", NULL);
	config.logger = optstring("logger", NULL);

	lua_close(L);//关闭虚拟机

	skynet_start(&config);//启动
	skynet_globalexit();//全局退出

	return 0;
}
