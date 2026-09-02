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

int Controller::delData(const std::string& varName)
{
	return cache->delData(varName).get();	// 同步等待磁盘删除完成
}

int Controller::selData(const std::string& varName, std::any& res)
{
	int resCode;
	cache->selData(varName, res, resCode);
	return resCode;
}

int Controller::persisVar(const std::string& varName)
{
	return cache->persisVar(varName).get();
}

int Controller::persisAll()
{
	return cache->persisVar().get();
}

int Controller::flushDisk()
{
	// 磁盘 IO 统一在磁盘线程执行，不占调用线程
	return diskThread->submit([this]() { return disk->flushDisk(); }, DISK_TASK).get();
}

int Controller::reWrite()
{
	return cache->reWrite().get();
}
