#pragma once
#ifndef _WRITE_PREF_MUTEX_H_
#define _WRITE_PREF_MUTEX_H_

#include <condition_variable>
#include <mutex>

/*
	写者优先读写锁：读锁共享（读读并发），写锁独占（写写互斥），
	有写者排队时新读者不插队（写者优先），保证读到的数据时效性高。
	接口命名对齐 shared_mutex，可直接配 shared_lock / unique_lock 使用。
*/
class WritePrefMutex {
private:
	std::mutex mtx;					// 保护状态
	std::condition_variable cv;		// 等待条件
	int readers = 0;				// 当前读者数
	bool writing = false;			// 是否有写者持有写锁
	bool writersWaiting = false;			// 写者是否等待

public:
	WritePrefMutex() = default;
	~WritePrefMutex() = default;

	WritePrefMutex(const WritePrefMutex&) = delete;
	WritePrefMutex& operator=(const WritePrefMutex&) = delete;

	// 读者：有写者在写，或有写者在排队 -> 等待（不插队）
	void lock_shared();
	void unlock_shared();

	// 写者：等所有读者走完、无其他写者
	void lock();
	void unlock();
};

#endif // !_WRITE_PREF_MUTEX_H_
