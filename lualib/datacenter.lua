--封装调用数据中心服务

local skynet = require "skynet"

local datacenter = {}

function datacenter.get(...)
	return skynet.call("DATACENTER", "lua", "QUERY", ...)
end

function datacenter.set(...)
	return skynet.call("DATACENTER", "lua", "UPDATE", ...)
end

function datacenter.wait(...)
	return skynet.call("DATACENTER", "lua", "WAIT", ...)
end

return datacenter

