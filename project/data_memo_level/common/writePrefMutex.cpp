#include "writePrefMutex.h"

WritePrefMutex::WritePrefMutex(int batchSize, int upDisEdge, int minDisEdge) : batchSize(batchSize), upDisEdge(upDisEdge), minDisEdge(minDisEdge) {}

// 读者：有写者在写，或有写者在排队 -> 等待（不插队）
// 例外：下界触发后持批名额的读者可插队（防止读饥饿）
void WritePrefMutex::lock_shared() {
	std::unique_lock<std::mutex> lk(mtx);
	// 无写者排队 -> 直接进；有写者排队 -> 需持有批名额才可进
	cv.wait(lk, [this]() { return !writing && (writersWaiting == 0 || batchRemain > 0); });
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
	// 读任务结束记账（读段与解锁一一对应，封装在此防止调用点漏记/误用）
	++readSubWrite;
	// 读太多且无写者排队：差距历史账清零，重新统计；同时关闭放行批
	if (readSubWrite >= upDisEdge && writersWaiting == 0) {
		readSubWrite = 0;
		batchRemain = 0;
	}
}

// 写者：等所有读者走完、无其他写者
void WritePrefMutex::lock() {
	std::unique_lock<std::mutex> lk(mtx);
	++writersWaiting;	// 进入排队队列（多写者各自记数，先获锁者不掩盖仍在排队的写者）
	// 当没有读者操作，且未进行写，则该写线程持锁(写串行，读写互斥)
	cv.wait(lk, [this](){ return !writing && !readers; });
	--writersWaiting;	// 已持锁，移出排队队列
	writing = true;
}

void WritePrefMutex::unlock() {
	std::lock_guard<std::mutex> lk(mtx);
	writing = false;
	// 写任务结束记账（写段与解锁一一对应，封装在此防止调用点漏记/误用）
	--readSubWrite;
	// 写太多，读被饿：开启一批读插队名额（仅未开启时，放行中的批次不叠加）
	if (readSubWrite <= minDisEdge && batchRemain == 0) {
		batchRemain += batchSize;
	}
	cv.notify_all();	// 叫醒等待的读者/写者
}
