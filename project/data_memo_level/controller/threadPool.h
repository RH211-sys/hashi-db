#pragma once
#ifndef _THREAD_POOL_
#define _THREAD_POOL_

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

/*
	线程池：固定线程数，提交任务后由空闲线程异步执行
	用途：处理持久化 IO 任务（单变量持久化、刷盘、数据重写等）
*/
class ThreadPool {
private:
	std::vector<std::thread> workers;				// 工作线程集合
	std::queue<std::function<void()>> tasks;		// 任务队列
	std::mutex mtx;									// 保护任务队列与 busy
	std::condition_variable cv;						// 通知空闲线程取任务
	std::condition_variable cvDone;					// 通知 waitAll 等待方
	size_t busy = 0;								// 正在执行的任务数
	bool stop = false;								// 停止标志（析构时置位）

	// 工作线程入口：取任务 -> 执行 -> 继续取，直到停止且队列清空
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
				task();	// 锁外执行，避免长时间持锁
			}
			catch (...) {
				// 任务抛异常不致命：吞掉继续干活，避免工作线程死亡
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
	// threadNum：工作线程数量
	explicit ThreadPool(size_t threadNum = 4) {
		workers.reserve(threadNum);
		for (size_t i = 0; i < threadNum; ++i) {
			workers.emplace_back(&ThreadPool::work, this);
		}
	}

	// 析构：停止接收新任务，处理完队列中已有任务后退出所有工作线程
	// 注意：持久化任务不能丢弃，否则数据丢失，所以这里选择处理完再退
	~ThreadPool() {
		{
			std::lock_guard<std::mutex> lock(mtx);
			stop = true;
		}
		cv.notify_all();
		for (auto& t : workers) {
			t.join();
		}
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// 提交任务，立即返回，由空闲工作线程异步执行
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

#endif
