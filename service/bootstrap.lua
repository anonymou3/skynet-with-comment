--引导服务

local skynet = require "skynet"
local harbor = require "skynet.harbor"

skynet.start(function()
	local standalone = skynet.getenv "standalone" --获取standalone配置
	--standalone代表该节点是否独立的，因为master节点在一个skynet网络内只有一个，所以是 "独立的""

	local launcher = assert(skynet.launch("snlua","launcher")) --启动launcher服务
	skynet.name(".launcher", launcher) --命名launcher服务为.launcher

	local harbor_id = tonumber(skynet.getenv "harbor")--获取harbor配置

	if harbor_id == 0 then --harbor配置为0，代表单节点网络
		assert(standalone ==  nil) -- 如果没有配置
		standalone = true --单节点当然也是独立的(master节点)
		skynet.setenv("standalone", "true") --设置到环境中

		local ok, slave = pcall(skynet.newservice, "cdummy")--启动cdummy服务
		if not ok then
			skynet.abort()
		end
		skynet.name(".cslave", slave)

	else
		if standalone then
			if not pcall(skynet.newservice,"cmaster") then
				skynet.abort()
			end
		end

		local ok, slave = pcall(skynet.newservice, "cslave")
		if not ok then
			skynet.abort()
		end
		skynet.name(".cslave", slave)
	end

	if standalone then
		local datacenter = skynet.newservice "datacenterd"
		skynet.name("DATACENTER", datacenter)
	end
	skynet.newservice "service_mgr"
	pcall(skynet.newservice,skynet.getenv "start" or "main")
	skynet.exit()
end)
