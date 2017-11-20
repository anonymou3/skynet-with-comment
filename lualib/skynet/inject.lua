local function getupvaluetable(u, func, unique)
	i = 1
	while true do

		local name, value = debug.getupvalue(func, i)--取上值
		if name == nil then --没有上值，结束循环
			return
		end
		local t = type(value) --取上值的类型

		if t == "table" then --
			u[name] = value
		elseif t == "function" then
			if not unique[value] then
				unique[value] = true
				getupvaluetable(u, value, unique)
			end
		end
		
		i=i+1
	end
end

return function(source, filename , ...)

	if filename then
		filename = "@" .. filename
	else
		filename = "=(load)"
	end
	local output = {}

	local function print(...)
		local value = { ... }
		for k,v in ipairs(value) do
			value[k] = tostring(v)
		end
		table.insert(output, table.concat(value, "\t"))
	end


	local u = {}
	local unique = {}
	local funcs = { ... }--传入的函数集合，一个是skynet.lua的dispatch_message 一个是skynet.register_protocol
	for k, func in ipairs(funcs) do
		getupvaluetable(u, func, unique)
	end

	local p = {}
	local proto = u.proto
	if proto then
		for k,v in pairs(proto) do
			local name, dispatch = v.name, v.dispatch
			if name and dispatch then
				local pp = {}
				p[name] = pp
				getupvaluetable(pp, dispatch, unique)
			end
		end
	end

	local env = setmetatable( { print = print , _U = u, _P = p}, { __index = _ENV })
	local func, err = load(source, filename, "bt", env)
	if not func then
		return { err }
	end
	local ok, err = xpcall(func, debug.traceback)
	if not ok then
		table.insert(output, err)
	end

	return output
end
