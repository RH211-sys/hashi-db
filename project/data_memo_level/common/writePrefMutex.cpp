#include "writePrefMutex.h"

WritePrefMutex::WritePrefMutex(int batchSize, int upDisEdge, int minDisEdge) : batchSize(batchSize), upDisEdge(upDisEdge), minDisEdge(minDisEdge) {}

// 读者：有写者在写，或有写者在排队 -> 等待（不插队）
// 例外：下界触发后持批名额的读者可插队（防止读饥饿）
void WritePrefMutex::lock_shared() {
	std::unique_lock<std::mutex> lk(mtx);
	// 无写者排队 -> 直接进；有写者排队 -> 需持有批名额才可进
	cv.wait(lk, [this]() { return !writing && (!writersWaiting || batchRemain > 0); });
	++readers;
	if (batchRemain > 0) {
		--batchRemain;	// 消耗一个插队名额
	}
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

// 读任务完成：差距+1，上界处理
void WritePrefMutex::onReadDone() {
	std::lock_guard<std::mutex> lk(mtx);
	++readSubWrite;
	// 读太多且无写者排队：差距历史账清零，重新统计；同时关闭放行批
	if (readSubWrite >= upDisEdge && !writersWaiting) {
		readSubWrite = 0;
		batchRemain = 0;
	}
}

// 写任务完成：差距-1，下界处理
void WritePrefMutex::onWriteDone() {
	std::lock_guard<std::mutex> lk(mtx);
	--readSubWrite;
	// 写太多，读被饿：开启一批读插队名额（仅未开启时，放行中的批次不叠加）
	if (readSubWrite <= minDisEdge && batchRemain == 0) {
		batchRemain += batchSize;
	}
	cv.notify_all();	// 唤醒等待的读者检查插队名额
}
