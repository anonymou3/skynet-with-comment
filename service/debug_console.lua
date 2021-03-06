local skynet = require "skynet"
local codecache = require "skynet.codecache"
local core = require "skynet.core"
local socket = require "socket"
local snax = require "snax"

local port = tonumber(...) --监听端口
local COMMAND = {}

local function format_table(t)
	local index = {}
	for k in pairs(t) do
		table.insert(index, k)
	end
	table.sort(index)
	local result = {}
	for _,v in ipairs(index) do
		table.insert(result, string.format("%s:%s",v,tostring(t[v])))
	end
	return table.concat(result,"\t")
end

local function dump_line(print, key, value)
	if type(value) == "table" then
		print(key, format_table(value))
	else
		print(key,tostring(value))
	end
end

local function dump_list(print, list)
	local index = {}
	for k in pairs(list) do
		table.insert(index, k)
	end
	table.sort(index)
	for _,v in ipairs(index) do
		dump_line(print, v, list[v])
	end
	print("OK")
end

local function split_cmdline(cmdline)
	local split = {}
	for i in string.gmatch(cmdline, "%S+") do
		table.insert(split,i)
	end
	return split
end

local function docmd(cmdline, print)
	local split = split_cmdline(cmdline)
	local cmd = COMMAND[split[1]]
	local ok, list
	if cmd then
		ok, list = pcall(cmd, select(2,table.unpack(split)))
	else
		ok, list = pcall(skynet.call,".launcher","lua", table.unpack(split))
	end

	if ok then
		if list then
			if type(list) == "string" then
				print(list)
			else
				dump_list(print, list)
			end
		else
			print("OK")
		end
	else
		print("Error:", list)
	end
end

--终端主循环
local function console_main_loop(stdin, print)
	socket.lock(stdin)
	print("Welcome to skynet console")
	while true do
		local cmdline = socket.readline(stdin, "\n") --读取一行命令
		if not cmdline then
			break
		end
		if cmdline ~= "" then
			docmd(cmdline, print) --执行命令
		end
	end
	socket.unlock(stdin)
end

skynet.start(function()
	local listen_socket = socket.listen ("127.0.0.1", port)--开始监听端口
	skynet.error("Start debug console at 127.0.0.1 " .. port)

	socket.start(listen_socket , function(id, addr)--开始接受连接
		local function print(...)
			local t = { ... }
			for k,v in ipairs(t) do
				t[k] = tostring(v)
			end
			socket.write(id, table.concat(t,"\t"))
			socket.write(id, "\n")
		end
		socket.start(id)--开始接受数据
		skynet.fork(console_main_loop, id , print)--fork一个协程运行主循环
	end)
end)

function COMMAND.help()
	return {
		help = "This help message",
		list = "List all the service",
		stat = "Dump all stats",
		info = "Info address : get service infomation",
		exit = "exit address : kill a lua service",
		kill = "kill address : kill service",
		mem = "mem : show memory status",
		gc = "gc : force every lua service do garbage collect",
		start = "lanuch a new lua service",
		snax = "lanuch a new snax service",
		clearcache = "clear lua code cache",
		service = "List unique service",
		task = "task address : show service task detail",
		inject = "inject address luascript.lua",
		logon = "logon address",
		logoff = "logoff address",
		log = "launch a new lua service with log",
	}
end

function COMMAND.clearcache()
	codecache.clear()
end

function COMMAND.start(...)
	local ok, addr = pcall(skynet.newservice, ...)
	if ok then
		return { [skynet.address(addr)] = ... }
	else
		return "Failed"
	end
end

function COMMAND.log(...)
	local ok, addr = pcall(skynet.call, ".launcher", "lua", "LOGLAUNCH", "snlua", ...)
	if ok then
		return { [skynet.address(addr)] = ... }
	else
		return "Failed"
	end
end

function COMMAND.snax(...)
	local ok, s = pcall(snax.newservice, ...)
	if ok then
		local addr = s.handle
		return { [skynet.address(addr)] = ... }
	else
		return "Failed"
	end
end

function COMMAND.service()
	return skynet.call("SERVICE", "lua", "LIST")
end

local function adjust_address(address)
	if address:sub(1,1) ~= ":" then--服务地址不是:0000000d形式
		--skynet.self() debug_console自己的地址
		address = bit32.replace( tonumber("0x" .. address), skynet.harbor(skynet.self()), 24, 8) --需要把地址的服务节点地址替换成debug_console所在节点
	end
	return address
end

function COMMAND.exit(address)
	skynet.send(adjust_address(address), "debug", "EXIT")
end

function COMMAND.inject(address, filename)
	address = adjust_address(address)--调整地址
	local f = io.open(filename, "rb")--打开文件
	if not f then
		return "Can't open " .. filename
	end
	local source = f:read "*a"--读取文件内容
	f:close()
	--skynet.lua注入了debug框架，所以所有服务可以都接收debug类型的消息
	return skynet.call(address, "debug", "RUN", source, filename)
end

function COMMAND.task(address)
	address = adjust_address(address)
	return skynet.call(address,"debug","TASK")
end

function COMMAND.info(address)
	address = adjust_address(address)
	return skynet.call(address,"debug","INFO")
end

function COMMAND.logon(address)
	address = adjust_address(address)
	core.command("LOGON", skynet.address(address))
end

function COMMAND.logoff(address)
	address = adjust_address(address)
	core.command("LOGOFF", skynet.address(address))
end
