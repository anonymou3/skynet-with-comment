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
local session_response = {}

local wakeup_session = {} --唤醒会话表
local sleep_session = {} --睡眠会话表

local watching_service = {} --监视服务表
local watching_session = {} --监视会话表：会话为key,服务为value
local dead_service = {} --死亡服务
local error_queue = {} --错误队列

-- suspend is function
local suspend

local function string_to_handle(str)
	return tonumber("0x" .. string.sub(str , 2))
end

----- monitor exit

local function dispatch_error_queue()
	local session = table.remove(error_queue,1)
	if session then
		local co = session_id_coroutine[session]
		session_id_coroutine[session] = nil
		return suspend(co, coroutine.resume(co, false))
	end
end

local function _error_dispatch(error_session, error_source)
	if error_session == 0 then
		-- service is down
		--  Don't remove from watching_service , because user may call dead service
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
		if watching_session[error_session] then
			table.insert(error_queue, error_session)
		end
	end
end

-- coroutine reuse

local coroutine_pool = {}	--协程池 保存当前没有任务执行的协程
local coroutine_yield = coroutine.yield --起个别名，同样提升性能
local coroutine_count = 0   --协程数目

local function co_create(f) --创建协程
	local co = table.remove(coroutine_pool) --取出表(协程池)中最后一个元素(协程)
	if co == nil then --取到的协程为空
		local print = print
		co = coroutine.create(function(...) --创建协程 哪里第一次resume协程？？因为协程创建后是挂起状态，即不自动运行
			f(...) --执行任务
			while true do --执行完任务后
				f = nil --置空原来的任务
				coroutine_pool[#coroutine_pool+1] = co --将协程保存在协程池中
				f = coroutine_yield "EXIT" --挂起协程，恢复时，得到的是新的任务
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
		--阻塞在" f(coroutine_yield()) "的协程将继续执行，...将作为参数传入f
	end
	return co
end

local function dispatch_wakeup()
	local co = next(wakeup_session)
	if co then
		wakeup_session[co] = nil
		local session = sleep_session[co]
		if session then
			session_id_coroutine[session] = "BREAK"
			return suspend(co, coroutine.resume(co, true, "BREAK"))
		end
	end
end

local function release_watching(address)
	local ref = watching_service[address]
	if ref then
		ref = ref - 1
		if ref > 0 then
			watching_service[address] = ref
		else
			watching_service[address] = nil
		end
	end
end

-- suspend is local function
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
		session_id_coroutine[param] = co --会话->协程
	elseif command == "SLEEP" then --协程因skynet.sleep或skynet.wait挂起
		session_id_coroutine[param] = co
		sleep_session[co] = param
	elseif command == "RETURN" then --协程因skynet.ret挂起
		local co_session = session_coroutine_id[co]
		local co_address = session_coroutine_address[co]
		if param == nil or session_response[co] then
			error(debug.traceback(co))
		end
		session_response[co] = true
		local ret
		if not dead_service[co_address] then
			ret = c.send(co_address, skynet.PTYPE_RESPONSE, co_session, param, size) ~= nil
		elseif size == nil then
			c.trash(param, size)
			ret = false
		end
		return suspend(co, coroutine.resume(co, ret))
	elseif command == "RESPONSE" then --协程因skynet.response挂起
		local co_session = session_coroutine_id[co]
		local co_address = session_coroutine_address[co]
		if session_response[co] then
			error(debug.traceback(co))
		end
		local f = param
		local function response(ok, ...)
			if ok == "TEST" then
				if dead_service[co_address] then
					release_watching(co_address)
					f = false
					return false
				else
					return true
				end
			end
			if not f then
				if f == false then
					f = nil
					return false
				end
				error "Can't response more than once"
			end

			local ret
			if not dead_service[co_address] then
				if ok then
					ret = c.send(co_address, skynet.PTYPE_RESPONSE, co_session, f(...)) ~= nil
				else
					ret = c.send(co_address, skynet.PTYPE_ERROR, co_session, "") ~= nil
				end
			else
				ret = false
			end
			release_watching(co_address)
			f = nil
			return ret
		end
		watching_service[co_address] = watching_service[co_address] + 1
		session_response[co] = response
		return suspend(co, coroutine.resume(co, response))
	elseif command == "EXIT" then --协程执行完当前任务
		-- coroutine exit
		local address = session_coroutine_address[co]
		release_watching(address)
		session_coroutine_id[co] = nil
		session_coroutine_address[co] = nil
		session_response[co] = nil
	elseif command == "QUIT" then --协程因skynet.exit挂起
		-- service exit
		return
	else --未知命令
		error("Unknown command : " .. command .. "\n" .. debug.traceback(co))
	end
	dispatch_wakeup()
	dispatch_error_queue()
end

function skynet.timeout(ti, func)--注册定时器 非阻塞API
	local session = c.command("TIMEOUT",tostring(ti)) --调用C库的执行命令函数(命令为注册定时器)
	assert(session)
	session = tonumber(session) --将会话串转为数字
	local co = co_create(func) --创建一个协程,此时创建的协程并没有运行
	assert(session_id_coroutine[session] == nil) --当前会话上没有对应的协程
	session_id_coroutine[session] = co --保存协程
end

function skynet.sleep(ti)
	local session = c.command("TIMEOUT",tostring(ti))
	assert(session)
	session = tonumber(session)
	local succ, ret = coroutine_yield("SLEEP", session)
	sleep_session[coroutine.running()] = nil
	assert(succ, ret)
	if ret == "BREAK" then
		return "BREAK"
	end
end

function skynet.yield()
	return skynet.sleep("0")
end

function skynet.wait()
	local session = c.genid()
	coroutine_yield("SLEEP", session)
	local co = coroutine.running()
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

function skynet.launch(...)
	local addr = c.command("LAUNCH", table.concat({...}," "))
	if addr then
		return string_to_handle(addr)
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
	watching_session[session] = service --会话为key,服务为value
	local succ, msg, sz = coroutine_yield("CALL", session)--挂起该协程
	watching_session[session] = nil --该协程恢复(resume)执行，去掉监视
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

function skynet.retpack(...)
	return skynet.ret(skynet.pack(...))
end

function skynet.wakeup(co)
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
		if co == "BREAK" then --？？
			session_id_coroutine[session] = nil
		elseif co == nil then --协程为空
			unknown_response(session, source, msg, sz) -- 未知的响应消息
		else
			session_id_coroutine[session] = nil --置空
			suspend(co, coroutine.resume(co, true, msg, sz)) --恢复正在等待响应的协程
		end
	else --其他服务发送来的请求
		local p = assert(proto[prototype], prototype) --取得对应的消息类别
		local f = p.dispatch --取得消息处理函数
		if f then --取得了消息处理函数
			local ref = watching_service[source] --根据消息来源取得引用数
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

function skynet.newservice(name, ...)
	return skynet.call(".launcher", "lua" , "LAUNCH", "snlua", name, ...)
end

function skynet.uniqueservice(global, ...)
	if global == true then
		return assert(skynet.call(".service", "lua", "GLAUNCH", ...))
	else
		return assert(skynet.call(".service", "lua", "LAUNCH", global, ...))
	end
end

function skynet.queryservice(global, ...)
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
