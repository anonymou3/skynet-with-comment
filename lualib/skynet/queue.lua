local skynet = require "skynet"
local coroutine = coroutine
local xpcall = xpcall
local traceback = debug.traceback
local table = table

function skynet.queue()
	local current_thread
	local ref = 0
	local thread_queue = {}

	return function(f, ...)
		local thread = coroutine.running() -- 获取当前运行的协程
		if current_thread and current_thread ~= thread then -- 如果是其他协程调用
			table.insert(thread_queue, thread) -- 插入到队列里
			skynet.wait()--阻塞其他协程
			assert(ref == 0)	-- current_thread == thread --当该协程恢复时，断言引用必然为0
		end
		current_thread = thread -- 保存第一次调用的协程

		ref = ref + 1 --增加引用计数
		local ok, err = xpcall(f, traceback, ...)--调用函数
		ref = ref - 1 --调用结束，减少引用计数
		if ref == 0 then
			current_thread = table.remove(thread_queue,1)--从队列里取出一个协程
			if current_thread then
				skynet.wakeup(current_thread)--唤醒协程运行
			end
		end
		assert(ok,err)
	end
	
end

return skynet.queue
