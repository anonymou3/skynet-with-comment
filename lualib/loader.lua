local args = {}

--string.gmatch (s, pattern)
--返回一个迭代器函数。 每次调用这个函数都会继续以 pattern （参见　§6.4.1） 对 s 做匹配，并返回所有捕获到的值。 如果 pattern 中没有指定捕获，则每次捕获整个 pattern
for word in string.gmatch(..., "%S+") do --这里pattern表达的意思是 非空白字符一次或多次
	table.insert(args, word)--插入到args中
end
--所以上面处理的结果就是，如果传入的参数是a b c d，则得到的args为:{['1']=a, ['2']=b, ['3']=c, ['4']=d}

SERVICE_NAME = args[1]	--传入的服务名，比如第一次启动snlua服务传入的是bootstrap

local main, pattern

local err = {}

for pat in string.gmatch(LUA_SERVICE, "([^;]+);*") do
	local filename = string.gsub(pat, "?", SERVICE_NAME)
	local f, msg = loadfile(filename)
	if not f then
		table.insert(err, msg)
	else
		pattern = pat
		main = f
		break
	end
end

if not main then
	error(table.concat(err, "\n"))
end

LUA_SERVICE = nil
package.path , LUA_PATH = LUA_PATH
package.cpath , LUA_CPATH = LUA_CPATH

local service_path = string.match(pattern, "(.*/)[^/?]+$")

if service_path then
	service_path = string.gsub(service_path, "?", args[1])
	package.path = service_path .. "?.lua;" .. package.path
	SERVICE_PATH = service_path
else
	local p = string.match(pattern, "(.*/).+$")
	SERVICE_PATH = p
end

if LUA_PRELOAD then
	local f = assert(loadfile(LUA_PRELOAD))
	f(table.unpack(args))
	LUA_PRELOAD = nil
end

main(select(2, table.unpack(args)))
