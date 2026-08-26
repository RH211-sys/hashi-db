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
	bool writersWaiting = false;	// 写者是否等待
	int readSubWrite = 0;			// 读任务 - 写任务，阈值，用来防止读饥饿
	int upDisEdge;					// 差距上限，若读太多readSubWrite到达上限，且没有写，则重置readSubWrite为0
	int minDisEdge;					// 差距下限，通常为负值，readSubWrite到达下限后，需要进行至少一批读并发
	int batchSize;					// 放行批大小（一次放行多少个读插队）
	int batchRemain = 0;			// 当前放行批的剩余名额，0表示未开启

public:
	explicit WritePrefMutex(int batchSize = 4, int upDisEdge = 10, int minDisEdge = -3);	// batchSize：下界触发时放行的一批读数量
	~WritePrefMutex() = default;

	WritePrefMutex(const WritePrefMutex&) = delete;
	WritePrefMutex& operator=(const WritePrefMutex&) = delete;

	// 读者：有写者在写，或有写者在排队 -> 等待（不插队）；下界触发时持有批名额的读者可插队
	void lock_shared();
	void unlock_shared();

	// 写者：等所有读者走完、无其他写者
	void lock();
	void unlock();

	// 读任务完成时调用：差距+1，上界处理
	void onReadDone();
	// 写任务完成时调用：差距-1，下界处理
	void onWriteDone();
};

#endif // !_WRITE_PREF_MUTEX_H_
