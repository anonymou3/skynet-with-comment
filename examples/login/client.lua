package.cpath = "luaclib/?.so"

local socket = require "clientsocket"
local crypt = require "crypt"
local bit32 = require "bit32"

--连接到登陆服
local fd = assert(socket.connect("127.0.0.1", 8001))

--向socket写入一行函数
local function writeline(fd, text)
	socket.send(fd, text .. "\n")--text加一个换行符
end

--从接收到的数据中解包出一行
local function unpack_line(text)
	local from = text:find("\n", 1, true)--找到一行
	if from then--找到换行符
		return text:sub(1, from-1), text:sub(from+1)--返回一行和剩余的text
	end
	return nil, text--返回空和传入的text
end

local last = ""--收到的数据

--使用解包函数f从数据中解出一条完整数据（完整的定义由协议确定，比如unpack_line是以一个换行符分隔）
--每调用一次unpack_f返回的匿名函数时，都会用传入的解包函数f从数据中解包出一条完整的数据
--如果没有解包到一条完整的数据，会不断循环接收数据->解包->休眠，直到解包出一条完整的数据
local function unpack_f(f)
	local function try_recv(fd, last)--尝试接收数据函数
		local result
		result, last = f(last)--解包数据
		if result then--拿到一条完整的数据
			return result, last--返回完整的数据和剩余数据
		end
		local r = socket.recv(fd)--读取数据
		if not r then--如果数据为空
			return nil, last--返回nil和当前的数据
		end
		if r == "" then--如果r为空字符串
			error "Server closed"--报告服务器关闭
		end
		return f(last .. r)--将剩余数据和收到的数据合并并继续解包
	end

	return function()
		while true do--循环直到拿到一条完整的数据
			local result
			result, last = try_recv(fd, last)
			if result then--拿到一条完整的数据
				return result--返回
			end
			socket.usleep(100)--休眠一会儿
		end
	end
end

--读取一行函数
local readline = unpack_f(unpack_line)

local challenge = crypt.base64decode(readline())--获取服务器发送过来的挑战码（8字节长的随机串，用于后续的握手验证）

local clientkey = crypt.randomkey()--生成一个随机的key

--1.利用 DH 密钥交换算法加密生成的clientkey
--2.再用base64编码
--3.发送到服务器
writeline(fd, crypt.base64encode(crypt.dhexchange(clientkey)))


local secret = crypt.dhsecret(crypt.base64decode(readline()), clientkey)

print("sceret is ", crypt.hexencode(secret))

local hmac = crypt.hmac64(challenge, secret)
writeline(fd, crypt.base64encode(hmac))

local token = {
	server = "sample",
	user = "hello",
	pass = "password",
}

local function encode_token(token)
	return string.format("%s@%s:%s",
		crypt.base64encode(token.user),
		crypt.base64encode(token.server),
		crypt.base64encode(token.pass))
end

local etoken = crypt.desencode(secret, encode_token(token))
local b = crypt.base64encode(etoken)
writeline(fd, crypt.base64encode(etoken))

local result = readline()
print(result)
local code = tonumber(string.sub(result, 1, 3))
assert(code == 200)
socket.close(fd)

local subid = crypt.base64decode(string.sub(result, 5))

print("login ok, subid=", subid)

----- connect to game server

local function send_request(v, session)
	local size = #v + 4
	local package = string.char(bit32.extract(size,8,8))..
		string.char(bit32.extract(size,0,8))..
		v..
		string.char(bit32.extract(session,24,8))..
		string.char(bit32.extract(session,16,8))..
		string.char(bit32.extract(session,8,8))..
		string.char(bit32.extract(session,0,8))

	socket.send(fd, package)
	return v, session
end

local function recv_response(v)
	local content = v:sub(1,-6)
	local ok = v:sub(-5,-5):byte()
	local session = 0
	for i=-4,-1 do
		local c = v:byte(i)
		session = session + bit32.lshift(c,(-1-i) * 8)
	end
	return ok ~=0 , content, session
end

local function unpack_package(text)
	local size = #text
	if size < 2 then
		return nil, text
	end
	local s = text:byte(1) * 256 + text:byte(2)
	if size < s+2 then
		return nil, text
	end

	return text:sub(3,2+s), text:sub(3+s)
end

local readpackage = unpack_f(unpack_package)

local function send_package(fd, pack)
	local size = #pack
	local package = string.char(bit32.extract(size,8,8))..
		string.char(bit32.extract(size,0,8))..
		pack

	socket.send(fd, package)
end

local text = "echo"
local index = 1

print("connect")
local fd = assert(socket.connect("127.0.0.1", 8888))
last = ""

local handshake = string.format("%s@%s#%s:%d", crypt.base64encode(token.user), crypt.base64encode(token.server),crypt.base64encode(subid) , index)
local hmac = crypt.hmac64(crypt.hashkey(handshake), secret)


send_package(fd, handshake .. ":" .. crypt.base64encode(hmac))

print(readpackage())
print("===>",send_request(text,0))
-- don't recv response
-- print("<===",recv_response(readpackage()))

print("disconnect")
socket.close(fd)

index = index + 1

print("connect again")
local fd = assert(socket.connect("127.0.0.1", 8888))
last = ""

local handshake = string.format("%s@%s#%s:%d", crypt.base64encode(token.user), crypt.base64encode(token.server),crypt.base64encode(subid) , index)
local hmac = crypt.hmac64(crypt.hashkey(handshake), secret)

send_package(fd, handshake .. ":" .. crypt.base64encode(hmac))

print(readpackage())
print("===>",send_request("fake",0))	-- request again (use last session 0, so the request message is fake)
print("===>",send_request("again",1))	-- request again (use new session)
print("<===",recv_response(readpackage()))
print("<===",recv_response(readpackage()))


print("disconnect")
socket.close(fd)

