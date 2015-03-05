local args = {}
for word in string.gmatch(..., "%S+") do --pattern的意思是 非空白字符一次或多次
	table.insert(args, word)--插入到args中
end

SERVICE_NAME = args[1]	--服务名bootstrap

local main, pattern

local err = {}

--string.gmatch (s, pattern)
--返回一个迭代器函数。 每次调用这个函数都会继续以 pattern （参见　§6.4.1） 对 s 做匹配，并返回所有捕获到的值。 如果 pattern 中没有指定捕获，则每次捕获整个 pattern

--作用：拆分LUA_SERVICE中配置的多个路径，并替换？为实际的脚本名，执行脚本
for pat in string.gmatch(LUA_SERVICE, "([^;]+);*") do	--pattern:捕获;之前的子串(子串不包含;)
	local filename = string.gsub(pat, "?", SERVICE_NAME) --将子串中的?替换为服务名
	local f, msg = loadfile(filename) --加载lua文件，如果没有语法错误， 则以函数形式返回编译好的代码块； 否则，返回 nil 加上错误消息。
	if not f then	--出错了
		table.insert(err, msg) --保存出错信息
	else	--成功了
		pattern = pat --路径
		main = f --返回的主函数(这里的主函数指的是要加载运行的lua代码)
		break --跳出循环
	end
end

if not main then --没有找到主函数
	error(table.concat(err, "\n")) --抛出错误
end

LUA_SERVICE = nil

package.path , LUA_PATH = LUA_PATH --设置package.path，置空LUA_PATH
package.cpath , LUA_CPATH = LUA_CPATH --设置package.cpath，置空LUA_CPATH

print(pattern)
local service_path = string.match(pattern, "(.*/)[^/?]+$") --捕获这种配置字符串 ./lualib/?/init.lua 捕获到./lualib/?/，大部分都是形如./service/?.lua
print(service_path)

if service_path then
	service_path = string.gsub(service_path, "?", args[1]) --替换?为服务名,如./lublib/example/
	package.path = service_path .. "?.lua;" .. package.path --将./lublib/example/?.lua添加到package.path
	SERVICE_PATH = service_path --设置SERVICE_PATH
else
	local p = string.match(pattern, "(.*/).+$") --捕获形如./service/?.lua，捕获到./service
	SERVICE_PATH = p --设置SERVICE_PATH
end

if LUA_PRELOAD then --如果有预加载的文件
	local f = assert(loadfile(LUA_PRELOAD)) --加载预加载文件
	f(table.unpack(args)) --运行预加载函数
	LUA_PRELOAD = nil --置空
end

main(select(2, table.unpack(args)))--运行主函数
