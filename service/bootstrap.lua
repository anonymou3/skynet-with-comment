--引导服务

local skynet = require "skynet"
local harbor = require "skynet.harbor"

skynet.start(function()
	local standalone = skynet.getenv "standalone" --获取standalone配置
	--standalone代表该节点是否独立的，因为master节点在一个skynet网络内只有一个，所以是 "独立的"

	local launcher = assert(skynet.launch("snlua","launcher")) --启动launcher服务,以后节点的启动都要通过该服务
	skynet.name(".launcher", launcher) --命名launcher服务为.launcher

	local harbor_id = tonumber(skynet.getenv "harbor")--获取harbor配置

	if harbor_id == 0 then --harbor配置为0，代表单节点网络
		assert(standalone ==  nil) --单节点网络下不能配置standalone
		standalone = true --单节点当然也是独立的(master节点)
		skynet.setenv("standalone", "true") --设置到环境中

		local ok, slave = pcall(skynet.newservice, "cdummy")--启动cdummy服务，拦截对外广播的全局名字变更
		if not ok then
			skynet.abort()
		end
		skynet.name(".cslave", slave)--命名服务

	else --多节点网络
		if standalone then --如果是master节点
			if not pcall(skynet.newservice,"cmaster") then --启动cmaster服务，做节点调度,协调组网
				skynet.abort()
			end
		end
		--无论是master节点还是slave节点，都有slave服务
		local ok, slave = pcall(skynet.newservice, "cslave") --启动cslave服务，用于节点间的消息转发，以及同步全局名字
		if not ok then
			skynet.abort()
		end
		skynet.name(".cslave", slave)--命名slave服务
	end

	--单节点网络会启动该服务，多节点网络在节点是master的时候会启动该服务
	if standalone then --单节点也需要启动datacentered服务
		local datacenter = skynet.newservice "datacenterd"
		skynet.name("DATACENTER", datacenter)
	end

	skynet.newservice "service_mgr" --管理UniqueService的服务

	pcall(skynet.newservice,skynet.getenv "start" or "main") --启动用户定义的main服务
	skynet.exit() --bootstrap服务退出
end)
