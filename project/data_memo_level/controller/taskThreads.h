#pragma once
#ifndef _TASK_THREADS_H_
#define _TASK_THREADS_H_

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

/*
	读线程池：固定线程数，仅调度读任务，由空闲线程并发执行回调
	写任务不走这里（写单线程串行），磁盘任务不走这里（磁盘单线程）
	读任务对象由上层封装，这里只负责取任务 -> 执行回调
*/
class readPool {
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
	explicit readPool(size_t threadNum = 4) {
		workers.reserve(threadNum);
		for (size_t i = 0; i < threadNum; ++i) {
			workers.emplace_back(&readPool::work, this);
		}
	}

	// 析构：停止接收新任务，处理完队列中已有任务后退出所有工作线程
	// 注意：已提交的读任务不能丢弃，否则上层 future 无人兑现，这里选择处理完再退
	~readPool() {
		{
			std::lock_guard<std::mutex> lock(mtx);
			stop = true;
		}
		cv.notify_all();
		for (auto& t : workers) {
			t.join();
		}
	}

	readPool(const readPool&) = delete;
	readPool& operator=(const readPool&) = delete;

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
class writeThread {
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
	writeThread() {
		worker = std::thread(&writeThread::work, this);
	}

	// 析构：停止接收新任务，处理完队列中已有任务后退出
	// 注意：写任务不能丢弃（脏数据落盘任务丢了会丢数据），处理完再退
	~writeThread() {
		{
			std::lock_guard<std::mutex> lock(mtx);
			stop = true;
		}
		cv.notify_all();
		worker.join();
	}

	// 防止线程对象进行拷贝构造
	writeThread(const writeThread&) = delete;
	writeThread& operator=(const writeThread&) = delete;

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
};

/*
	磁盘线程：单线程串行执行磁盘 IO 任务（持久化/刷盘/重写/删除）
	缓存调用后异步操作：提交任务立即返回，调用者不等待
	inDisk/delDisk/文件只有本线程访问，天然串行，无需锁
	磁盘任务对象由上层封装，这里只负责取任务 -> 执行回调
*/
class diskThread {
private:
	std::thread worker;								// 磁盘单线程
	std::queue<std::function<void()>> tasks;		// 磁盘任务队列
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
				task();	// 锁外执行回调，磁盘 IO 全程锁外
			}
			catch (...) {
				// 磁盘任务异常不致命：吞掉继续干活，避免磁盘线程死亡
			}
		}
	}

public:
	diskThread() {
		worker = std::thread(&diskThread::work, this);
	}

	// 析构：停止接收新任务，处理完队列中已有任务后退出
	// 注意：磁盘任务不能丢弃（刷盘/重写丢了数据不完整），处理完再退
	~diskThread() {
		{
			std::lock_guard<std::mutex> lock(mtx);
			stop = true;
		}
		cv.notify_all();
		worker.join();
	}

	diskThread(const diskThread&) = delete;
	diskThread& operator=(const diskThread&) = delete;

	// 提交磁盘任务，立即返回，由磁盘线程串行执行
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
};

#endif
