local skynet = require "skynet"
local socket = require "socket"
local socketdriver = require "socketdriver"

-- channel support auto reconnect , and capture socket error in request/response transaction
-- { host = "", port = , auth = function(so) , response = function(so) session, data }

local socket_channel = {} --socket channel模块
local channel = {}-- socket channel 父类
local channel_socket = {}
local channel_meta = { __index = channel }--socket channel元表
local channel_socket_meta = {
	__index = channel_socket,
	__gc = function(cs)
		local fd = cs[1]
		cs[1] = false
		if fd then
			socket.shutdown(fd)
		end
	end
}

-- socket channel error方法
local socket_error = setmetatable({}, {__tostring = function() return "[Error: socket]" end })	-- alias for error object
socket_channel.error = socket_error

--socket channel 创建channel方法
function socket_channel.channel(desc)
	local c = {
		__host = assert(desc.host),
		__port = assert(desc.port),
		__backup = desc.backup,
		__auth = desc.auth,
		__response = desc.response,	-- It's for session mode 用于会话模式
		__request = {},	-- request seq { response func or session }	-- It's for order mode 用于顺序模式
		__thread = {}, -- coroutine seq or session->coroutine map
		__result = {}, -- response result { coroutine -> result }
		__result_data = {},
		__connecting = {},
		__sock = false,
		__closed = false,
		__authcoroutine = false,
		__nodelay = desc.nodelay,
	}

	return setmetatable(c, channel_meta)--设置元表
end

local function close_channel_socket(self)
	if self.__sock then
		local so = self.__sock
		self.__sock = false
		-- never raise error
		pcall(socket.close,so[1])
	end
end

local function wakeup_all(self, errmsg)
	if self.__response then
		for k,co in pairs(self.__thread) do
			self.__thread[k] = nil
			self.__result[co] = socket_error
			self.__result_data[co] = errmsg
			skynet.wakeup(co)
		end
	else
		for i = 1, #self.__request do
			self.__request[i] = nil
		end
		for i = 1, #self.__thread do
			local co = self.__thread[i]
			self.__thread[i] = nil
			self.__result[co] = socket_error
			self.__result_data[co] = errmsg
			skynet.wakeup(co)
		end
	end
end



local function dispatch_by_session(self)
	local response = self.__response
	-- response() return session
	while self.__sock do
		local ok , session, result_ok, result_data = pcall(response, self.__sock)
		if ok and session then
			local co = self.__thread[session]
			self.__thread[session] = nil
			if co then
				self.__result[co] = result_ok
				self.__result_data[co] = result_data
				skynet.wakeup(co)
			else
				skynet.error("socket: unknown session :", session)
			end
		else
			close_channel_socket(self)
			local errormsg
			if session ~= socket_error then
				errormsg = session
			end
			wakeup_all(self, errormsg)
		end
	end
end

local function pop_response(self)
	return table.remove(self.__request, 1), table.remove(self.__thread, 1)
end

local function push_response(self, response, co)
	if self.__response then
		-- response is session
		self.__thread[response] = co
	else
		-- response is a function, push it to __request
		table.insert(self.__request, response)
		table.insert(self.__thread, co)
	end
end

local function dispatch_by_order(self)
	while self.__sock do
		local func, co = pop_response(self)
		if func == nil then
			if not socket.block(self.__sock[1]) then
				close_channel_socket(self)
				wakeup_all(self)
			end
		else
			local ok, result_ok, result_data = pcall(func, self.__sock)
			if ok then
				self.__result[co] = result_ok
				self.__result_data[co] = result_data
				skynet.wakeup(co)
			else
				close_channel_socket(self)
				local errmsg
				if result_ok ~= socket_error then
					errmsg = result_ok
				end
				self.__result[co] = socket_error
				self.__result_data[co] = errmsg
				skynet.wakeup(co)
				wakeup_all(self, errmsg)
			end
		end
	end
end

--派发函数入口
local function dispatch_function(self)
	if self.__response then --如果有response函数
		return dispatch_by_session--按session派发：模式2
	else
		return dispatch_by_order--按顺序派发：模式1
	end
end
--连接备份服务器
local function connect_backup(self)
	if self.__backup then
		--遍历所有数据，连接成功一个则返回
		for _, addr in ipairs(self.__backup) do
			local host, port
			if type(addr) == "table" then
				host, port = addr.host, addr.port
			else
				host = addr
				port = self.__port
			end
			skynet.error("socket: connect to backup host", host, port)
			local fd = socket.open(host, port)
			if fd then
				self.__host = host
				self.__port = port
				return fd
			end
		end
	end
end

local function connect_once(self)
	--关闭状态检查
	if self.__closed then
		return false
	end
	--断言检查
	assert(not self.__sock and not self.__authcoroutine)
	--
	local fd = socket.open(self.__host, self.__port)--建立连接
	--连接备份服务器
	if not fd then
		fd = connect_backup(self)
		if not fd then
			return false
		end
	end
	if self.__nodelay then
		socketdriver.nodelay(fd)
	end

	self.__sock = setmetatable( {fd} , channel_socket_meta )--设置__sock字段

	skynet.fork(dispatch_function(self), self)--fork一个协程做派发

	if self.__auth then--存在验证函数
		self.__authcoroutine = coroutine.running()--保存正在运行的coroutine
		local ok , message = pcall(self.__auth, self)--调用验证函数
		if not ok then
			close_channel_socket(self)
			if message ~= socket_error then
				self.__authcoroutine = false
				skynet.error("socket: auth failed", message)
			end
		end
		self.__authcoroutine = false
		if ok and not self.__sock then
			-- auth may change host, so connect again
			-- 验证可能改变服务器，所以重连一次
			return connect_once(self)
		end
		return ok
	end

	return true
end
--尝试进行连接
local function try_connect(self , once)
	local t = 0
	while not self.__closed do --只要没有设置关闭标记，则一直尝试进行连接
		if connect_once(self) then
			if not once then--如果不是尝试一次连接，打印日志
				skynet.error("socket: connect to", self.__host, self.__port)
			end
			return true
		elseif once then --但是如果设置了once标记，一次连接失败，则返回false
			return false
		end
		--重新连接
		if t > 1000 then
			skynet.error("socket: try to reconnect", self.__host, self.__port)
			skynet.sleep(t)
			t = 0
		else
			skynet.sleep(t)
		end
		t = t + 100
	end
end

-- 检查连接
-- 几种情况：
-- 当没有连接，此时__sock为空，如果__closed状态标记为ture,则返回false,否则返回nil
-- 当存在连接，此时__sock不为空，（1）如果当前验证协程为空，则返回true（2）当前验证协程不为空，并且当前运行的协程与保存的协程号一致，则返回true
-- (3)当前验证协程不为空，但当前运行的协程与保存的协程号不一致（可能发生吗？？可能吧，比如在另外一个协程内调用了check_connection），如果__closed状态标记为ture,则返回false,否则返回nil

local function check_connection(self)
	if self.__sock then
		local authco = self.__authcoroutine
		if not authco then
			return true
		end
		if authco == coroutine.running() then
			-- authing
			return true
		end
	end
	if self.__closed then 
		return false
	end
end

local function block_connect(self, once)
	--检查连接
	local r = check_connection(self)
	if r ~= nil then
		return r
	end

	--如果已经尝试过进行连接（表示从其他协程尝试进行连接），则将协程加入到__connecting表内，然后将协程挂起
	if #self.__connecting > 0 then
		-- connecting in other coroutine
		local co = coroutine.running()
		table.insert(self.__connecting, co)
		skynet.wait()
	else--首次尝试进行连接
		self.__connecting[1] = true --标记
		try_connect(self, once) --尝试连接
		self.__connecting[1] = nil -- 取消标记
		--唤醒其他挂起的协程
		for i=2, #self.__connecting do 
			local co = self.__connecting[i]
			self.__connecting[i] = nil
			skynet.wakeup(co)
		end
	end

	--再检查一次连接
	r = check_connection(self)
	if r == nil then
		error(string.format("Connect to %s:%d failed", self.__host, self.__port))
	else
		return r
	end
end

--建立连接
function channel:connect(once)
	-- 关闭状态检查
	if self.__closed then
		self.__closed = false
	end

	return block_connect(self, once)
end

local function wait_for_response(self, response)
	local co = coroutine.running()
	push_response(self, response, co)
	skynet.wait()

	local result = self.__result[co]
	self.__result[co] = nil
	local result_data = self.__result_data[co]
	self.__result_data[co] = nil

	if result == socket_error then
		error(socket_error)
	else
		assert(result, result_data)
		return result_data
	end
end

-- 发起请求
function channel:request(request, response)
	assert(block_connect(self, true))	-- connect once

	if not socket.write(self.__sock[1], request) then
		close_channel_socket(self)
		wakeup_all(self)
		error(socket_error)
	end

	if response == nil then
		-- no response
		return
	end

	return wait_for_response(self, response)
end

function channel:response(response)
	assert(block_connect(self))

	return wait_for_response(self, response)
end
--关闭channel
function channel:close()
	if not self.__closed then --检查关闭状态标记
		self.__closed = true --设置关闭状态标记
		close_channel_socket(self) --关闭连接
	end
end

function channel:changehost(host, port)
	self.__host = host
	if port then
		self.__port = port
	end
	if not self.__closed then
		close_channel_socket(self)
	end
end

function channel:changebackup(backup)
	self.__backup = backup
end

channel_meta.__gc = channel.close

local function wrapper_socket_function(f)
	return function(self, ...)
		local result = f(self[1], ...)
		if not result then
			error(socket_error)
		else
			return result
		end
	end
end

channel_socket.read = wrapper_socket_function(socket.read)
channel_socket.readline = wrapper_socket_function(socket.readline)

return socket_channel
