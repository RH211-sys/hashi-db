#pragma once
#ifndef _TASK_THREADS_H_
#define _TASK_THREADS_H_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

/*
	读线程池：固定线程数，仅调度读任务，由空闲线程并发执行回调
	写任务不走这里（写单线程串行），磁盘任务不走这里（磁盘单线程）
	读任务对象由上层封装，这里只负责取任务 -> 执行回调
*/
class ReadPool {
	friend class DiskThread;
	friend class WriteThread;
private:
	std::vector<std::thread> workers;				// 工作线程集合
	std::queue<std::function<void()>> tasks;		// 读任务队列
	std::mutex mtx;									// 保护任务队列与 busy
	std::condition_variable cv;						// 通知空闲线程取任务
	std::condition_variable cvDone;					// 通知 waitAll 等待方
	size_t busy = 0;								// 正在执行的任务数
	bool stop = false;								// 停止标志（析构时置位）

	// 工作线程入口：取任务 -> 执行回调 -> 继续取，直到停止且队列清空
	void work() {
		while (true) {
			std::function<void()> task;
			{
				std::unique_lock<std::mutex> lock(mtx);
				cv.wait(lock, [this] { return stop || !tasks.empty(); });
				if (stop && tasks.empty()) {
					return;	// 停止且队列清空，本线程退出
				}
				task = std::move(tasks.front());
				tasks.pop();
				++busy;
			}
			try {
				task();	// 锁外执行回调，避免长时间持锁
			}
			catch (...) {
				// 读任务异常不致命：吞掉继续干活，避免工作线程死亡
			}
			{
				std::lock_guard<std::mutex> lock(mtx);
				--busy;
				if (busy == 0 && tasks.empty()) {
					cvDone.notify_all();	// 全部任务完成，唤醒 waitAll
				}
			}
		}
	}

public:
	// threadNum：工作线程数量（读写差距阈值放行的批大小 = 线程数）
	explicit ReadPool(size_t threadNum = 4) {
		workers.reserve(threadNum);
		for (size_t i = 0; i < threadNum; ++i) {
			workers.emplace_back(&ReadPool::work, this);
		}
	}

	// 析构：停止接收新任务，处理完队列中已有任务后退出所有工作线程
	// 注意：已提交的读任务不能丢弃，否则上层 future 无人兑现，这里选择处理完再退
	~ReadPool() {
		{
			std::lock_guard<std::mutex> lock(mtx);
			stop = true;
		}
		cv.notify_all();
		for (auto& t : workers) {
			t.join();
		}
	}

	ReadPool(const ReadPool&) = delete;
	ReadPool& operator=(const ReadPool&) = delete;

	// 提交读任务，立即返回，由空闲工作线程异步执行回调
	// future 扩展：后续读任务需要拿结果时，在此新增返回 future 的接口
	void push(std::function<void()> task) {
		{
			std::lock_guard<std::mutex> lock(mtx);
			if (stop) {
				return;	// 已停止，拒绝新任务
			}
			tasks.push(std::move(task));
		}
		cv.notify_one();
	}

	/*
		提交任务并返回 future：只有调用者 get() 时阻塞等待结果，其他任务不受影响
		任务抛异常时 packaged_task 自动将异常存入 future，get() 会重新抛出
		模板直接接收可调用对象（lambda 等），避免 std::function 模板推导失败
		std::invoke_result_t<F>：F 无参调用得到的返回类型
	*/
	template <typename F>
	std::future<std::invoke_result_t<F>> submit(F task) {
		using R = std::invoke_result_t<F>;	// 任务返回类型，由可调用对象自动推导
		auto ptask = std::make_shared<std::packaged_task<R()>>(std::move(task));
		std::future<R> fut = ptask->get_future();
		{
			std::lock_guard<std::mutex> lock(mtx);
			if (stop) {
				return {};	// 已停止，拒绝新任务
			}
			tasks.push([ptask]() { (*ptask)(); });	// 通过 shared_ptr 延长任务对象生命周期
		}
		cv.notify_one();
		return fut;
	}

	// 阻塞等待所有已提交任务执行完毕（队列清空且 busy 为 0）
	void waitAll() {
		std::unique_lock<std::mutex> lock(mtx);
		cvDone.wait(lock, [this] { return tasks.empty() && busy == 0; });
	}
};

/*
	写线程：单线程串行执行缓存写任务
	写必须串行（避免unordered_map 结构竞争 + rehash 迭代器失效），天然无需任务锁
	写任务对象由其他模块封装，这里只负责取任务 -> 执行回调
*/
class WriteThread {
	friend class ReadPool;
	friend class DiskThread;
private:
	std::thread worker;								// 写线程
	std::queue<std::function<void()>> tasks;		// 写任务队列
	std::mutex mtx;									// 保护任务队列（push 线程与 worker 并发访问）
	std::condition_variable cv;						// 条件变量，通知线程取任务
	bool stop = false;								// 停止标志（析构时置位）

	// 线程入口：取任务 -> 执行回调 -> 继续取，直到停止且队列清空
	void work() {
		while (true) {
			std::function<void()> task;
			{
				std::unique_lock<std::mutex> lock(mtx);
				cv.wait(lock, [this] { return stop || !tasks.empty(); });
				if (stop && tasks.empty()) {
					return;	// 停止且队列清空，本线程退出
				}
				task = std::move(tasks.front());
				tasks.pop();
			}
			try {
				task();	// 锁外执行回调，避免长时间持锁
			}
			catch (...) {
				// 写任务异常不致命：吞掉继续干活，避免单写线程死亡
			}
		}
	}

public:
	WriteThread() {
		worker = std::thread(&WriteThread::work, this);
	}

	// 析构：停止接收新任务，处理完队列中已有任务后退出
	// 注意：写任务不能丢弃（脏数据落盘任务丢了会丢数据），处理完再退
	~WriteThread() {
		{
			std::lock_guard<std::mutex> lock(mtx);
			stop = true;
		}
		cv.notify_all();
		worker.join();
	}

	// 防止线程对象进行拷贝构造
	WriteThread(const WriteThread&) = delete;
	WriteThread& operator=(const WriteThread&) = delete;

	// 提交写任务，立即返回，由写线程串行执行
	void push(std::function<void()> task) {
		{
			std::lock_guard<std::mutex> lock(mtx);
			if (stop) {
				return;	// 已停止，拒绝新任务
			}
			tasks.push(std::move(task));
		}
		cv.notify_one();
	}

	/*
		提交任务并返回 future：只有调用者 get() 时阻塞等待结果，其他任务不受影响
		任务抛异常时 packaged_task 自动将异常存入 future，get() 会重新抛出
		模板直接接收可调用对象（lambda 等），避免 std::function 模板推导失败
		std::invoke_result_t<F>：F 无参调用得到的返回类型
	*/
	template <typename F>
	std::future<std::invoke_result_t<F>> submit(F task) {
		using R = std::invoke_result_t<F>;	// 任务返回类型，由可调用对象自动推导
		auto ptask = std::make_shared<std::packaged_task<R()>>(std::move(task));
		std::future<R> fut = ptask->get_future();
		{
			std::lock_guard<std::mutex> lock(mtx);
			if (stop) {
				return {};	// 已停止，拒绝新任务
			}
			tasks.push([ptask]() { (*ptask)(); });	// 通过 shared_ptr 延长任务对象生命周期
		}
		cv.notify_one();
		return fut;
	}
};


/*
	任务类型：用于区分磁盘线程中的两类任务
	DISK_TASK：磁盘 IO 任务（慢，毫秒级：文件读写），进磁盘任务队列
	CACHE_TASK：缓存操作任务（快，微秒级：访问 cache_db），进缓存任务队列，优先调度
*/
const int DISK_TASK = 1;	// 磁盘任务
const int CACHE_TASK = 2;	// 缓存任务

// 磁盘线程观测快照（供性能归因，非功能）：提交量 / 队列积压 / worker 忙碌
struct DiskQueueStat {
	long long pushCnt = 0;		// 提交（push/submit）次数
	double avgDepth = 0;		// 提交时双队列合计平均深度
	long long maxDepth = 0;		// 提交时队列深度峰值
	long long runCnt = 0;		// worker 已执行任务数
	long long busyUs = 0;		// worker 执行任务总耗时（µs）
};

/*
	磁盘线程：单线程串行执行磁盘 IO 任务（持久化/刷盘/重写/删除）
	缓存调用后异步操作：提交任务立即返回，调用者不等待
	inDisk/delDisk/文件只有本线程访问，天然串行，无需锁
	磁盘任务对象由上层封装，这里只负责取任务 -> 执行回调
*/
class DiskThread {
	friend class ReadPool;
	friend class WriteThread;
private:
	std::thread worker;								// 磁盘单线程
	std::queue<std::function<void()>> cacheTasks;	// 缓存操作任务队列（快：访问 cache_db，微秒级）
	std::queue<std::function<void()>> diskTasks;	// 磁盘 IO 任务队列（慢：文件读写，毫秒级）
	std::mutex mtx;									// 保护任务队列（push 线程与 worker 并发访问）
	std::condition_variable cv;						// 条件变量，通知线程取任务
	int taskGap = 0;			// 任务差距（处理缓存 - 处理磁盘）
	int upEdge = 15;			// 差距上限：缓存任务堆积到上限时，若有磁盘任务则让位调度一个
	int lowEdge = -5;			// 差距下限：磁盘任务处理过多时，重置差距
	bool stop = false;								// 停止标志（析构时置位）
	// 观测统计（relaxed，供性能归因）：队列积压与 worker 忙碌占比
	std::atomic<long long> statPushCnt{ 0 };	// 提交（push/submit）次数
	std::atomic<long long> statDepthSum{ 0 };	// 提交时双队列总深度累加
	std::atomic<long long> statDepthMax{ 0 };	// 提交时队列深度峰值
	std::atomic<long long> statRunCnt{ 0 };		// worker 已执行任务数
	std::atomic<long long> statBusyUs{ 0 };		// worker 执行任务总耗时（µs）
	// 提交观测记账（须持锁调用：队列 size 读安全）：push 次数 / 深度累加 / 峰值
	void notePush() {
		statPushCnt.fetch_add(1, std::memory_order_relaxed);
		long long depth = static_cast<long long>(diskTasks.size() + cacheTasks.size());
		statDepthSum.fetch_add(depth, std::memory_order_relaxed);
		long long maxD = statDepthMax.load(std::memory_order_relaxed);
		if (depth > maxD) statDepthMax.store(depth, std::memory_order_relaxed);
	}

	// 线程入口：取任务 -> 执行回调 -> 继续取，直到停止且队列清空
	void work() {
		while (true) {
			std::function<void()> task;
			{
				std::unique_lock<std::mutex> lock(mtx);
				cv.wait(lock, [this] { return stop || !cacheTasks.empty() || !diskTasks.empty(); });
				if (stop && cacheTasks.empty() && diskTasks.empty()) {
					return;	// 停止且队列清空，本线程退出
				}
				// 调度决策：缓存任务优先；差距到上限且有待处理磁盘任务时，让位给磁盘
				if (!cacheTasks.empty() && !(taskGap >= upEdge && !diskTasks.empty())) {
					task = std::move(cacheTasks.front());
					cacheTasks.pop();
					++taskGap;	// 处理缓存任务 +1
					if (taskGap >= upEdge && diskTasks.empty()) {
						taskGap = 0;	// 到达上限且无磁盘任务：重置差距，继续处理缓存
					}
				}
				else if (!diskTasks.empty()) {
					task = std::move(diskTasks.front());
					diskTasks.pop();
					--taskGap;	// 处理磁盘任务 -1
					if (taskGap <= lowEdge) {
						taskGap = 0;	// 到达下限：重置差距
					}
				}
				else {
					continue;	// 理论不可达：wait 已保证队列非空
				}
			}
			auto runT0 = std::chrono::steady_clock::now();	// worker 执行耗时观测起点
			try {
				task();	// 锁外执行回调，磁盘 IO 全程锁外
			}
			catch (...) {
				// 磁盘任务异常不致命：吞掉继续干活，避免磁盘线程死亡
			}
			statBusyUs.fetch_add(static_cast<long long>(
				std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - runT0).count()),
				std::memory_order_relaxed);
			statRunCnt.fetch_add(1, std::memory_order_relaxed);
		}
	}

public:
	DiskThread() {
		worker = std::thread(&DiskThread::work, this);
	}

	// 析构：停止接收新任务，处理完队列中已有任务后退出
	// 注意：磁盘任务不能丢弃（刷盘/重写丢了数据不完整），处理完再退
	~DiskThread() {
		{
			std::lock_guard<std::mutex> lock(mtx);
			stop = true;
		}
		cv.notify_all();
		worker.join();
	}

	DiskThread(const DiskThread&) = delete;
	DiskThread& operator=(const DiskThread&) = delete;

	// 提交任务，立即返回，由磁盘线程串行执行
	// taskType：任务类型（DISK_TASK 磁盘任务 / CACHE_TASK 缓存任务）
	void push(std::function<void()> task, int taskType) {
		{
			std::lock_guard<std::mutex> lock(mtx);
			if (stop) {
				return;	// 已停止，拒绝新任务
			}
			if (taskType == DISK_TASK) {
				diskTasks.push(std::move(task));
			}
			else if (taskType == CACHE_TASK) {
				cacheTasks.push(std::move(task));	// 默认按缓存任务处理
			}
			notePush();	// 观测：提交时队列深度
		}
		cv.notify_one();
	}
	/*
		提交任务并返回 future：只有调用者 get() 时阻塞等待结果，其他任务不受影响
		任务抛异常时 packaged_task 自动将异常存入 future，get() 会重新抛出
		taskType：任务类型（DISK_TASK 磁盘任务 / CACHE_TASK 缓存任务）
		模板直接接收可调用对象（lambda 等），避免 std::function 模板推导失败
		std::invoke_result_t<F>：F 无参调用得到的返回类型
	*/
	template <typename F>
	std::future<std::invoke_result_t<F>> submit(F task, int taskType) {
		using R = std::invoke_result_t<F>;	// 任务返回类型，由可调用对象自动推导
		auto ptask = std::make_shared<std::packaged_task<R()>>(std::move(task));
		std::future<R> fut = ptask->get_future();
		{
			std::lock_guard<std::mutex> lock(mtx);
			if (stop) {
				return {};	// 已停止，拒绝新任务
			}
			if (taskType == DISK_TASK) {
				diskTasks.push([ptask]() { (*ptask)(); });	// 通过 shared_ptr 延长任务对象生命周期
			}
			else if (taskType == CACHE_TASK) {
				cacheTasks.push([ptask]() { (*ptask)(); });	// 默认按缓存任务处理
			}
			notePush();	// 观测：提交时队列深度
		}
		cv.notify_one();
		return fut;
	}

	// 观测快照：提交次数 / 平均与峰值队列深度 / worker 执行任务数与总耗时
	DiskQueueStat getQueueStat() const {
		DiskQueueStat s;
		s.pushCnt = statPushCnt.load(std::memory_order_relaxed);
		s.maxDepth = statDepthMax.load(std::memory_order_relaxed);
		s.avgDepth = (s.pushCnt > 0)
			? static_cast<double>(statDepthSum.load(std::memory_order_relaxed)) / s.pushCnt : 0.0;
		s.runCnt = statRunCnt.load(std::memory_order_relaxed);
		s.busyUs = statBusyUs.load(std::memory_order_relaxed);
		return s;
	}
};

#endif
