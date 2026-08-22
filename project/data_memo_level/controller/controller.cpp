#include "controller.h"


Controller::Controller(long long memoSize, long long maxSize, size_t poolThreadNum)
{
	this->cache = std::make_unique<Cache>(memoSize);
	this->disk = std::make_unique<Disk>(maxSize);
	this->pool = std::make_unique<ThreadPool>(poolThreadNum);
	cache->setDisk(disk.get());	// setDisk/setCache 接收裸指针，用 .get() 取出
	disk->setCache(cache.get());
}

int Controller::delData(const std::string& varName) {
	return 0;
}

int Controller::selData(const std::string& varName, std::any& res) {
	return 0;
}


int Controller::persisVar(const std::string& varName) {
	return 0;
}
int Controller::flushDisk() {
	return 0;
}
int Controller::reWrite() {
	return 0;
}