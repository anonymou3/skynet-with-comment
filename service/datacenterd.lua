--数据中心服务
--数据中心服务是启动在master节点上的,用来在整个 skynet 网络做跨节点的数据共享
--适用范围：
--1.跨节点服务地址的存储(如果你只需要简单的服务地址管理，UniqueService 做的更好，它会在每个节点都缓存查询结果)
--2.如果是一份数据，很多地方都需要获得它，那么用一个服务装下这组数据，提供一组查询接口即可
local skynet = require "skynet"

local command = {}
local database = {}
local wait_queue = {}
local mode = {}

local function query(db, key, ...)
	if key == nil then
		return db
	else
		return query(db[key], ...)
	end
end

function command.QUERY(key, ...)
	local d = database[key]
	if d then
		return query(d, ...)
	end
end

local function update(db, key, value, ...)
	if select("#",...) == 0 then
		local ret = db[key]
		db[key] = value
		return ret, value
	else
		if db[key] == nil then
			db[key] = {}
		end
		return update(db[key], value, ...)
	end
end

local function wakeup(db, key1, ...)
	if key1 == nil then
		return
	end
	local q = db[key1]
	if q == nil then
		return
	end
	if q[mode] == "queue" then
		db[key1] = nil
		if select("#", ...) ~= 1 then
			-- throw error because can't wake up a branch
			for _,response in ipairs(q) do
				response(false)
			end
		else
			return q
		end
	else
		-- it's branch
		return wakeup(q , ...)
	end
end

function command.UPDATE(...)
	local ret, value = update(database, ...)
	if ret or value == nil then
		return ret
	end
	local q = wakeup(wait_queue, ...)
	if q then
		for _, response in ipairs(q) do
			response(true,value)
		end
	end
end

local function waitfor(db, key1, key2, ...)
	if key2 == nil then
		-- push queue
		local q = db[key1]
		if q == nil then
			q = { [mode] = "queue" }
			db[key1] = q
		else
			assert(q[mode] == "queue")
		end
		table.insert(q, skynet.response())
	else
		local q = db[key1]
		if q == nil then
			q = { [mode] = "branch" }
			db[key1] = q
		else
			assert(q[mode] == "branch")
		end
		return waitfor(q, key2, ...)
	end
end

skynet.start(function()
	skynet.dispatch("lua", function (_, _, cmd, ...)
		if cmd == "WAIT" then
			local ret = command.QUERY(...)
			if ret then
				skynet.ret(skynet.pack(ret))
			else
				waitfor(wait_queue, ...)
			end
		else
			local f = assert(command[cmd])
			skynet.ret(skynet.pack(f(...)))
		end
	end)
end)
