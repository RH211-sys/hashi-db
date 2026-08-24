#include "writePrefMutex.h"

// 读者：有写者在写，或有写者在排队 -> 等待（不插队）
void WritePrefMutex::lock_shared() {
	std::unique_lock<std::mutex> lk(mtx);
	// 写者未持锁操作，且无等待中的写者，则进来的读线程持锁(写优先,读共享)
	cv.wait(lk, [this](){ return !writing && !writersWaiting; });
	++readers;
}

void WritePrefMutex::unlock_shared() {
	std::lock_guard<std::mutex> lk(mtx);
	if (--readers == 0) {
		cv.notify_all();	// 最后一名读者离开，叫醒等待的写者
	}
}

// 写者：等所有读者走完、无其他写者
void WritePrefMutex::lock() {
	std::unique_lock<std::mutex> lk(mtx);
	writersWaiting = true;
	// 当没有读者操作，且未进行写，则该写线程持锁(写串行，读写互斥)
	cv.wait(lk, [this](){ return !writing && !readers; });
	writersWaiting = false;
	writing = true;
}

void WritePrefMutex::unlock() {
	{
		std::lock_guard<std::mutex> lk(mtx);
		writing = false;
	}
	cv.notify_all();	// 叫醒等待的读者/写者
}
