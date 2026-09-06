#include "controller.h"


Controller::Controller(long long memoSize, long long maxSize, size_t poolThreadNum, std::string dbName)
{
	cache = std::make_shared<Cache>(memoSize);
	disk = std::make_shared<Disk>(maxSize, dbName);
	readPool = std::make_shared<ReadPool>(poolThreadNum);
	writeThread = std::make_shared<WriteThread>();
	diskThread = std::make_shared<DiskThread>();

	// 注入依赖：Cache 持 Disk/DiskThread 的 shared_ptr，Disk 持 Cache 的 weak_ptr（无循环引用）
	cache->bind(diskThread, disk, readPool, writeThread);
	disk->setCache(cache);
}

std::future<int> Controller::delData(const std::string& varName)
{
	// 删除任务已在缓存模块内交写线程执行，直接透传 future，调用方不阻塞
	return cache->delData(varName);
}

std::future<SelResult> Controller::selData(const std::string& varName)
{
	// 查询任务已在缓存模块内交读线程池执行，直接透传 future，调用方不阻塞
	return cache->selData(varName);
}

std::future<int> Controller::persisVar(const std::string& varName)
{
	// 刷盘任务已在缓存模块内交磁盘线程执行，直接透传 future，调用方不阻塞
	return cache->persisVar(varName);
}

std::future<int> Controller::persisAll()
{
	// 刷盘任务已在缓存模块内交磁盘线程执行，直接透传 future，调用方不阻塞
	return cache->persisVar();
}

std::future<int> Controller::flushDisk()
{
	// 刷盘是磁盘模块方法（清理过期 + 落盘脏数据）：磁盘 IO 统一在磁盘线程执行，调用方不阻塞
	return diskThread->submit([this]() { return disk->flushDisk(); }, DISK_TASK);
}

std::future<int> Controller::reWrite()
{
	// 重写任务已在缓存模块内交磁盘线程执行，直接透传 future，调用方不阻塞
	return cache->reWrite();
}

CacheStat Controller::getStat() const
{
	// 统计快照读自缓存模块（原子计数），直接透传
	return cache->getStat();
}

DiskIoStat Controller::getIoStat() const
{
	// 磁盘 IO 阶段统计：透传磁盘模块
	return disk->getIoStat();
}

DiskQueueStat Controller::getQueueStat() const
{
	// 磁盘线程队列/忙碌统计：透传磁盘线程
	return diskThread->getQueueStat();
}
