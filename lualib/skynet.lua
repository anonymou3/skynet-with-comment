local c = require "skynet.core"	--加载C库

--下面为什么要这么写？？
--可以想到的是这样写可以提高性能
local tostring = tostring
local tonumber = tonumber
local coroutine = coroutine
local assert = assert
local pairs = pairs
local pcall = pcall

local profile = require "profile" --加载C库

coroutine.resume = profile.resume --替换成C库中的函数
coroutine.yield = profile.yield --替换成C库中的函数

local proto = {}  --保存注册的消息类别信息
local skynet = {
	--预定义的消息类别id
	-- read skynet.h 同skynet.h中的定义一致
	PTYPE_TEXT = 0,
	PTYPE_RESPONSE = 1,
	PTYPE_MULTICAST = 2,
	PTYPE_CLIENT = 3,
	PTYPE_SYSTEM = 4,
	PTYPE_HARBOR = 5,
	PTYPE_SOCKET = 6,
	PTYPE_ERROR = 7,
	PTYPE_QUEUE = 8,	-- use in deprecated mqueue, use skynet.queue instead
	PTYPE_DEBUG = 9,
	PTYPE_LUA = 10,
	PTYPE_SNAX = 11,
}

-- code cache
skynet.cache = require "skynet.codecache"

function skynet.register_protocol(class) --注册新的消息类别，传入的class是个table
	local name = class.name --类别名
	local id = class.id --类别id
	assert(proto[name] == nil) -- 该消息之前没有注册
	assert(type(name) == "string" and type(id) == "number" and id >=0 and id <=255)--判断参数是否合法
	proto[name] = class --通过名字可以索引到该协议
	proto[id] = class --通过id也可以索引到该
end

local session_id_coroutine = {} --会话->协程表
local session_coroutine_id = {} --协程->会话表 
local session_coroutine_address = {} --协程->消息源表
local session_response = {} --协程->是否回应表

local wakeup_session = {} --协程->是否唤醒表
local sleep_session = {} --协程->会话表（协程休眠时使用）

local watching_service = {} --消息源->引用数表
local watching_session = {} --会话->服务表
local dead_service = {} --消息源->是否挂掉表
local error_queue = {} --错误队列(保存了出错的会话)

-- suspend is function
local suspend

local function string_to_handle(str) --字符串转换为数字句柄
	return tonumber("0x" .. string.sub(str , 2))
end

----- monitor exit

local function dispatch_error_queue()
	local session = table.remove(error_queue,1)--从错误队列中取出会话
	if session then --会话有效
		local co = session_id_coroutine[session] --取出协程
		session_id_coroutine[session] = nil
		return suspend(co, coroutine.resume(co, false)) --恢复协程执行
	end
end

local function _error_dispatch(error_session, error_source) --错误消息派发函数
	if error_session == 0 then --会话为0
		-- service is down
		--  Don't remove from watching_service , because user may call dead service
		-- 服务已经挂了
		-- 不要从watching_service中移除，因为用户可能会调用死亡的服务
		if watching_service[error_source] then 
			dead_service[error_source] = true
		end
		for session, srv in pairs(watching_session) do
			if srv == error_source then
				table.insert(error_queue, session)
			end
		end
	else
		-- capture an error for error_session
		-- 从错误会话中捕获错误
		if watching_session[error_session] then
			table.insert(error_queue, error_session)
		end
	end
end

-- coroutine reuse
-- 协程重用
local coroutine_pool = {}	--协程池 保存当前没有任务执行的协程
local coroutine_yield = coroutine.yield --起个别名，同样提升性能
local coroutine_count = 0   --协程数目

local function co_create(f) --创建协程或者从协程池拿出一个协程
	--什么时候调用co_create:
	--1:skynet.timeout
	--2:skynet.fork
	--3:raw_dispatch_message,其他服务发送来的请求
	local co = table.remove(coroutine_pool) --取出表(协程池)中最后一个元素(协程)
	if co == nil then --取到的协程为空
		local print = print
		co = coroutine.create(function(...) --创建协程 哪里第一次resume协程？(在raw_dispatch_message)因为协程创建后是挂起状态，即不自动运行
			f(...) --执行任务
			while true do --执行完任务后
				f = nil --释放原来的任务
				coroutine_pool[#coroutine_pool+1] = co --将协程保存在协程池中
				f = coroutine_yield "EXIT" --挂起协程，恢复时，得到的是新的任务。
				--这样的效果就是调用者不知道得到的协程到底是新建的还是从协程池里取出的
				--因为从调用者看来，使用的方式都是一样：调用co_create得到一个协程，然后调用一次resume传入参数

				f(coroutine_yield())--执行新任务前，还要挂起一下，以此来获取新任务参数
			end
		end)
		coroutine_count = coroutine_count + 1 --增加协程数计数器
		if coroutine_count > 1024 then --协程数超过1024
			skynet.error("May overload, create 1024 task") --可能超载了
			coroutine_count = 0 --清空协程数计数器 只清空不做其他处理么？
		end
	else --取到的协程不为空，使用协程池内的协程执行任务（其实是函数，这里形象点说成任务）
		coroutine.resume(co, f)--恢复协程，传入新任务
		--原来协程阻塞在" f = coroutine_yield "EXIT" "
		--resume后，f得到了新的值
		--然后阻塞在" f(coroutine_yield()) "
		--当返回了co，在外面调用coroutine.resume(co,...)
		--阻塞在" f(coroutine_yield()) "的协程将继续执行，...将作为参数传入f，f得以执行（执行新的任务）
	end
	return co
end

local function dispatch_wakeup() --唤醒协程队列
	local co = next(wakeup_session) --取出key
	if co then
		wakeup_session[co] = nil --置空
		local session = sleep_session[co] --获得会话
		if session then --会话合法
			session_id_coroutine[session] = "BREAK" --设置打断标志
			return suspend(co, coroutine.resume(co, true, "BREAK")) --恢复协程执行
		end
	end
end

local function release_watching(address) --释放引用数
	local ref = watching_service[address] --获取当前引用数
	if ref then --如果不为空
		ref = ref - 1 --自减
		if ref > 0 then --如果大于0
			watching_service[address] = ref --设置为当前值
		else
			watching_service[address] = nil --小于等于0则直接置空
		end
	end
end

-- suspend is local function
-- coroutine.resume一般是包含在suspend函数内的
-- 因此在协程挂起后，可以做一些统筹的工作
function suspend(co, result, command, param, size) --当协程执行完当前请求或者调用了阻塞API(会yield自己)时
	--result：协程yield或者协程主函数返回时的第一个参数
	--command,param,size 后续参数
	if not result then --如果result不为true
		local session = session_coroutine_id[co] --取出协程对应的会话
		if session then -- coroutine may fork by others (session is nil) --会话不为空
			--注意这里：Lua将false和nil看作是false，其他所有都是true
			local addr = session_coroutine_address[co] --取出协程对应的消息源
			if session ~= 0 then --如果消息不等于0，代表消息是通过skynet.call发送的
				-- only call response error
				c.send(addr, skynet.PTYPE_ERROR, session, "") --向消息源发送一条出错消息
			end
			session_coroutine_id[co] = nil --置空
			session_coroutine_address[co] = nil --置空
		end
		error(debug.traceback(co,tostring(command))) --报错
	end
	if command == "CALL" then --协程因skynet.call挂起，此时param为session
		session_id_coroutine[param] = co --保存协程（会在分发响应消息时取出协程恢复运行）


	elseif command == "SLEEP" then --协程因skynet.sleep或skynet.wait挂起
		session_id_coroutine[param] = co --保存会话
		sleep_session[co] = param --保存参数(实际上是会话)


	elseif command == "RETURN" then --协程因skynet.ret挂起
		local co_session = session_coroutine_id[co] --获取会话
		local co_address = session_coroutine_address[co] --获取消息源
		if param == nil or session_response[co] then --参数为空或者会话已经回应了
			error(debug.traceback(co))--报错
		end
		session_response[co] = true --设置回应标志，表示该会话已经回应
		local ret
		if not dead_service[co_address] then --如果消息源没有挂
			ret = c.send(co_address, skynet.PTYPE_RESPONSE, co_session, param, size) ~= nil --回应消息
			--返回值不为空，代表发送成功
		elseif size == nil then --消息源挂了 并且消息大小为空
			c.trash(param, size)--回收消息的内存占用
			ret = false 
		end
		return suspend(co, coroutine.resume(co, ret)) --继续恢复协程执行，表面上看skynet.ret好像没有阻塞，实际上还是发生了一次阻塞动作（挂起）
	

	elseif command == "RESPONSE" then --协程因skynet.response挂起
		local co_session = session_coroutine_id[co] --获取会话
		local co_address = session_coroutine_address[co] --获取消息源
		if session_response[co] then --如果该会话已经回应了
			error(debug.traceback(co)) --报错
		end
		local f = param --该参数实际上是打包函数，默认是skynet.pack

		--定义一个闭包
		local function response(ok, ...)

			if ok == "TEST" then --如果OK传入的是"TEST",得到回应地址(消息源)的有效性
				if dead_service[co_address] then --如果消息源挂了
					release_watching(co_address) --释放引用数
					f = false --
					return false
				else --消息源正常
					return true 
				end
			end

			if not f then --如果打包函数为空
				if f == false then --如果是false
					f = nil --置空
					return false --返回false
				end
				--因为调用一次闭包后，f就被设置为nil了，所以不是false,就是nil了，也就是调用了多次
				error "Can't response more than once" --不能回应多次
			end

			local ret
			if not dead_service[co_address] then --如果消息源没有挂掉
				if ok then --如果传入的是true
					ret = c.send(co_address, skynet.PTYPE_RESPONSE, co_session, f(...)) ~= nil --回应消息
				else
					ret = c.send(co_address, skynet.PTYPE_ERROR, co_session, "") ~= nil --给请求者抛出一个异常
				end
			else
				ret = false
			end
			release_watching(co_address) --释放引用数
			f = nil --置空打包函数
			return ret
		end

		watching_service[co_address] = watching_service[co_address] + 1 --增加引用数
		session_response[co] = response --保存闭包
		return suspend(co, coroutine.resume(co, response)) --恢复协程执行，返回闭包


	elseif command == "EXIT" then --协程执行完当前任务退出，此时协程已经进入协程池
		-- coroutine exit
		local address = session_coroutine_address[co] --获取消息源
		release_watching(address) --释放引用
		session_coroutine_id[co] = nil --置空会话
		session_coroutine_address[co] = nil --置空消息源
		session_response[co] = nil --置空是否响应

	elseif command == "QUIT" then --协程因skynet.exit挂起
		-- service exit
		return

	else --未知命令
		error("Unknown command : " .. command .. "\n" .. debug.traceback(co))
	end

	dispatch_wakeup() --调度唤醒协程
	dispatch_error_queue() --调度错误队列
end

function skynet.timeout(ti, func)--注册定时器 非阻塞API
	local session = c.command("TIMEOUT",tostring(ti)) --调用C库的执行命令函数(命令为注册定时器)
	assert(session)
	session = tonumber(session) --将会话串转为数字
	local co = co_create(func) --创建一个协程,此时创建的协程并没有运行
	assert(session_id_coroutine[session] == nil) --当前会话上没有对应的协程
	session_id_coroutine[session] = co --保存协程
end

function skynet.sleep(ti) --休眠ti个单位时间
	local session = c.command("TIMEOUT",tostring(ti))--向框架注册一个定时器
	assert(session)
	session = tonumber(session)
	local succ, ret = coroutine_yield("SLEEP", session) --挂起协程
	sleep_session[coroutine.running()] = nil --协程恢复执行
	assert(succ, ret)
	if ret == "BREAK" then
		return "BREAK"
	end
end

function skynet.yield()
	return skynet.sleep("0")
end

function skynet.wait() --挂起协程
	local session = c.genid() --生成一个session
	coroutine_yield("SLEEP", session) --挂起协程
	local co = coroutine.running()--协程恢复
	sleep_session[co] = nil
	session_id_coroutine[session] = nil
end

local function globalname(name, handle)
	local c = string.sub(name,1,1)
	assert(c ~= ':')
	if c == '.' then
		return false
	end

	assert(#name <= 16)	-- GLOBALNAME_LENGTH is 16, defined in skynet_harbor.h
	assert(tonumber(name) == nil)	-- global name can't be number

	local harbor = require "skynet.harbor"

	harbor.globalname(name, handle)

	return true
end

function skynet.register(name)
	if not globalname(name) then
		c.command("REG", name)
	end
end

function skynet.name(name, handle)
	if not globalname(name, handle) then
		c.command("NAME", name .. " " .. skynet.address(handle))
	end
end

local self_handle
function skynet.self()
	if self_handle then
		return self_handle
	end
	self_handle = string_to_handle(c.command("REG"))
	return self_handle
end

function skynet.localname(name)
	local addr = c.command("QUERY", name)
	if addr then
		return string_to_handle(addr)
	end
end

function skynet.launch(...) --启动一个服务,通过调用C库
	local addr = c.command("LAUNCH", table.concat({...}," ")) --将参数用空格连接起来(返回的是字符串)
	if addr then
		return string_to_handle(addr)--返回数字句柄
	end
end

function skynet.now()
	return tonumber(c.command("NOW"))
end

function skynet.starttime()
	return tonumber(c.command("STARTTIME"))
end

function skynet.time()
	return skynet.now()/100 + skynet.starttime()	-- get now first would be better
end

function skynet.exit()
	fork_queue = {}	-- no fork coroutine can be execute after skynet.exit
	skynet.send(".launcher","lua","REMOVE",skynet.self())
	-- report the sources that call me
	for co, session in pairs(session_coroutine_id) do
		local address = session_coroutine_address[co]
		if session~=0 and address then
			c.redirect(address, 0, skynet.PTYPE_ERROR, session, "")
		end
	end
	-- report the sources I call but haven't return
	local tmp = {}
	for session, address in pairs(watching_session) do
		tmp[address] = true
	end
	for address in pairs(tmp) do
		c.redirect(address, 0, skynet.PTYPE_ERROR, 0, "")
	end
	c.command("EXIT")
	-- quit service
	coroutine_yield "QUIT"
end

function skynet.kill(name)
	if type(name) == "number" then
		skynet.send(".launcher","lua","REMOVE",name)
		name = skynet.address(name)
	end
	c.command("KILL",name)
end

function skynet.getenv(key)
	local ret = c.command("GETENV",key)
	if ret == "" then
		return
	else
		return ret
	end
end

function skynet.setenv(key, value)
	c.command("SETENV",key .. " " ..value)
end

function skynet.send(addr, typename, ...) --非阻塞发送消息
	local p = proto[typename] --先根据名字取得对应的消息类别(table)
	return c.send(addr, p.id, 0 , p.pack(...)) --先用p.pack打包数据，然后调用c库发送消息
end

skynet.genid = assert(c.genid)

skynet.redirect = function(dest,source,typename,...)
	return c.redirect(dest, source, proto[typename].id, ...)
end

--以下函数使用的都是C库中的函数
skynet.pack = assert(c.pack)	
skynet.packstring = assert(c.packstring)
skynet.unpack = assert(c.unpack)
skynet.tostring = assert(c.tostring)

local function yield_call(service, session)
	watching_session[session] = service --保存服务
	local succ, msg, sz = coroutine_yield("CALL", session)--挂起该协程
	watching_session[session] = nil --该协程恢复(resume)执行，去掉监视（在哪恢复的？在raw_dispatch_message的if prototype == 1分支）
	assert(succ, debug.traceback()) --如果失败了，打印堆栈
	return msg,sz --返回消息，消息大小
end

function skynet.call(addr, typename, ...)--阻塞发送消息
	local p = proto[typename] --先根据名字取得对应的消息类别(table)
	local session = c.send(addr, p.id , nil , p.pack(...)) --p.pack打包消息，然后发送消息，保存返回的会话
	if session == nil then
		error("call to invalid address " .. skynet.address(addr))
	end
	return p.unpack(yield_call(addr, session))--挂起调用，解包返回的(消息，消息大小)
end

function skynet.rawcall(addr, typename, msg, sz)
	local p = proto[typename]
	local session = assert(c.send(addr, p.id , nil , msg, sz), "call to invalid address")
	return yield_call(addr, session)
end

function skynet.ret(msg, sz)
	msg = msg or ""
	return coroutine_yield("RETURN", msg, sz)
end

function skynet.response(pack)
	pack = pack or skynet.pack
	return coroutine_yield("RESPONSE", pack)
end

function skynet.retpack(...) --skynet.ret(skynet.pack(...))简写
	return skynet.ret(skynet.pack(...))
end

function skynet.wakeup(co) --唤醒一个协程
	if sleep_session[co] and wakeup_session[co] == nil then
		wakeup_session[co] = true
		return true
	end
end

function skynet.dispatch(typename, func) --注册消息处理函数
	local p = assert(proto[typename],tostring(typename)) --获取消息类别
	assert(p.dispatch == nil, tostring(typename)) --该消息类别还没有注册消息处理函数
	p.dispatch = func --注册消息处理函数
end

local function unknown_request(session, address, msg, sz, prototype)
	skynet.error(string.format("Unknown request (%s): %s", prototype, c.tostring(msg,sz)))
	error(string.format("Unknown session : %d from %x", session, address))
end

function skynet.dispatch_unknown_request(unknown)
	local prev = unknown_request
	unknown_request = unknown
	return prev
end

local function unknown_response(session, address, msg, sz)
	print("Response message :" , c.tostring(msg,sz))
	error(string.format("Unknown session : %d from %x", session, address))
end

function skynet.dispatch_unknown_response(unknown)
	local prev = unknown_response
	unknown_response = unknown
	return prev
end

local fork_queue = {} --创建的协程队列

local tunpack = table.unpack

function skynet.fork(func,...) --创建一个新的协程
	local args = { ... } --所有的参数
	local co = co_create(function() --创建协程
		func(tunpack(args))
	end)
	table.insert(fork_queue, co) --插入fork队列
end

local function raw_dispatch_message(prototype, msg, sz, session, source, ...)
	-- skynet.PTYPE_RESPONSE = 1, read skynet.h
	if prototype == 1 then -- 响应的消息（自己先发请求，等待响应，此时会挂起相应的协程）
		local co = session_id_coroutine[session] --取出session对应的协程
		if co == "BREAK" then --协程被wakeup
			session_id_coroutine[session] = nil
		elseif co == nil then --协程为空
			unknown_response(session, source, msg, sz) -- 未知的响应消息
		else
			session_id_coroutine[session] = nil --置空
			suspend(co, coroutine.resume(co, true, msg, sz)) --恢复正在等待响应的协程
		end
	else --其他服务发送来的请求
		local p = assert(proto[prototype], prototype) --取得对应的消息类别
		local f = p.dispatch --取得消息派发函数
		if f then --取得了消息派发函数
			local ref = watching_service[source] --根据消息源取得引用数
			if ref then
				watching_service[source] = ref + 1 --增加引用数
			else
				watching_service[source] = 1 --设置引用数为1
			end
			local co = co_create(f) --创建一个新的协程
			session_coroutine_id[co] = session --保存会话
			session_coroutine_address[co] = source --保存消息源
			suspend(co, coroutine.resume(co, session,source, p.unpack(msg,sz, ...)))--恢复协程
			--如果是新建的协程，则将参数通过resume直接传入主函数
			--如果是从协程池取出的，协程先resume一次获取到消息处理函数（co_create内的else分支），然后在这里再resume一次获取到具体的参数
		else --没有找到对应的消息处理函数
			unknown_request(session, source, msg, sz, proto[prototype]) --未知请求
		end
	end
end

local function dispatch_message(...) --派发消息回调
	local succ, err = pcall(raw_dispatch_message,...) --调用原始的派发消息函数
	while true do --死循环
		local key,co = next(fork_queue)--从fork队列取出一个协程
		if co == nil then--没有协程
			break--跳出循环
		end
		fork_queue[key] = nil
		local fork_succ, fork_err = pcall(suspend,co,coroutine.resume(co))
		if not fork_succ then
			if succ then
				succ = false
				err = tostring(fork_err)
			else
				err = tostring(err) .. "\n" .. tostring(fork_err)
			end
		end
	end
	assert(succ, tostring(err))
end

function skynet.newservice(name, ...) --启动一个LUA服务,实际上是给launcher发送消息实现的
	return skynet.call(".launcher", "lua" , "LAUNCH", "snlua", name, ...) 
end

function skynet.uniqueservice(global, ...) --启动一个唯一的LUA服务，如果global等于true,则是全局唯一的
	if global == true then
		return assert(skynet.call(".service", "lua", "GLAUNCH", ...))
	else
		return assert(skynet.call(".service", "lua", "LAUNCH", global, ...))
	end
end

function skynet.queryservice(global, ...) --查询服务
	if global == true then
		return assert(skynet.call(".service", "lua", "GQUERY", ...))
	else
		return assert(skynet.call(".service", "lua", "QUERY", global, ...))
	end
end

function skynet.address(addr)
	if type(addr) == "number" then --如果是数字
		return string.format(":%08x",addr) --将数字地址转换为字符串
	else
		return tostring(addr) --转换任何类型到人可阅读的字符串形式
	end
end

function skynet.harbor(addr)
	return c.harbor(addr)
end

function skynet.error(...)
	local t = {...}
	for i=1,#t do
		t[i] = tostring(t[i])
	end
	return c.error(table.concat(t, " "))
end

----- register protocol 在此预先注册lua层使用的消息类别
do
	local REG = skynet.register_protocol

	REG {
		name = "lua",	--skynet中最常用的消息类别
		id = skynet.PTYPE_LUA,
		pack = skynet.pack,
		unpack = skynet.unpack,
	}

	REG {
		name = "response", --响应类型的消息
		id = skynet.PTYPE_RESPONSE,
	}

	REG {
		name = "error",
		id = skynet.PTYPE_ERROR,
		unpack = function(...) return ... end,
		dispatch = _error_dispatch,
	}
end

local init_func = {} --初始化函数表

function skynet.init(f, name) --注册初始化函数，这些函数会在调用启动函数前被调用
	assert(type(f) == "function") --传入的f必须是函数
	if init_func == nil then --如果初始化函数表为空
		f() --直接调用传入的函数
	else --初始化函数表不为空
		if name == nil then --如果名字为空
			table.insert(init_func, f) --直接插入
		else
			assert(init_func[name] == nil) --名字不为空，判断没有该名字对应的函数
			init_func[name] = f --插入函数，key为名字
		end
	end
end

local function init_all() --初始化所有
	local funcs = init_func --引用初始化函数表
	init_func = nil --释放引用，init_func指向的表会自动释放掉
	for k,v in pairs(funcs) do --遍历初始化函数表
		v() --执行函数
	end
end

local function init_template(start) --初始化模版
	init_all() --初始化所有
	init_func = {} --重新设置初始化函数表,因为init_all()会释放init_func
	start() --调用启动函数
	init_all() --再初始化所有一次,why??
end

local function init_service(start) --初始化服务
	local ok, err = xpcall(init_template, debug.traceback, start)
	if not ok then 
		skynet.error("init service failed: " .. tostring(err))
		skynet.send(".launcher","lua", "ERROR")
		skynet.exit()
	else
		skynet.send(".launcher","lua", "LAUNCHOK")
	end
end

function skynet.start(start_func)  --注册启动函数
	c.callback(dispatch_message) --重新设置上下文的回调函数
	skynet.timeout(0, function() --0个单位时间，调用func
		init_service(start_func)
	end)
end

function skynet.filter(f ,start_func)
	c.callback(function(...)
		dispatch_message(f(...))
	end)
	skynet.timeout(0, function()
		init_service(start_func)
	end)
end

function skynet.forward_type(map, start_func)
	c.callback(function(ptype, msg, sz, ...)
		local prototype = map[ptype]
		if prototype then
			dispatch_message(prototype, msg, sz, ...)
		else
			dispatch_message(ptype, msg, sz, ...)
			c.trash(msg, sz)
		end
	end, true)
	skynet.timeout(0, function()
		init_service(start_func)
	end)
end

function skynet.endless()
	return c.command("ENDLESS")~=nil
end

function skynet.abort()
	c.command("ABORT")
end

function skynet.monitor(service, query)
	local monitor
	if query then
		monitor = skynet.queryservice(true, service)
	else
		monitor = skynet.uniqueservice(true, service)
	end
	assert(monitor, "Monitor launch failed")
	c.command("MONITOR", string.format(":%08x", monitor))
	return monitor
end

function skynet.mqlen()
	return tonumber(c.command "MQLEN")
end

function skynet.task(ret)
	local t = 0
	for session,co in pairs(session_id_coroutine) do
		if ret then
			ret[session] = debug.traceback(co)
		end
		t = t + 1
	end
	return t
end

function skynet.term(service)
	return _error_dispatch(0, service)
end

local function clear_pool()
	coroutine_pool = {}
end

-- Inject internal debug framework
-- 注入内部的debug框架
local debug = require "skynet.debug"
debug(skynet, {
	dispatch = dispatch_message,
	clear = clear_pool,
})

return skynet
