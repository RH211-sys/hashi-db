#include "cache.h"


Cache::Cache(const long long memoSize, const int batchSize, const int upDisEdge, const int minDisEdge)
{
	this->memoSize = memoSize;
	this->curSize = 0;
	this->rwMutex = std::make_unique<WritePrefMutex>(batchSize, upDisEdge, minDisEdge);
}

void Cache::setDisk(std::shared_ptr<Disk> d)
{
	this->disk = d;
}

void Cache::setDiskThread(std::shared_ptr<DiskThread> t)
{
	this->diskThread = t;
}

int Cache::selData(const std::string& varName, std::any& res) {
	// 获取读共享锁
	rwMutex->lock_shared();
	// 命中缓存直接返回（缓存数据必定最新，且效率高）
	if (cache_db.contains(varName)) {
		res = cache_db.at(varName).entity;	// 返回值的实体，不是整个 Val
		rwMutex->unlock_shared();
		return SUCCESS;
	}
	// 未命中：先解锁，再走磁盘查询，避免持锁做磁盘 IO
	rwMutex->unlock_shared();

	// 提交磁盘读任务，只有发起者 get() 阻塞等待结果，其他线程不受影响
	std::future<int> fut = diskThread->submit([this, &varName, &res]() {
		return disk->selData(varName, res);
	});
	int code = fut.get();
	if (code != SUCCESS) {
		return code;	// 磁盘也找不到
	}

	// 回填缓存前重新校验 key（可能已被并发修改/删除），校验通过才回填
	{
		rwMutex->lock();	// 回填是写操作，拿写锁
		if (!cache_db.contains(varName)) {
			Val v;
			v.entity = res;
			v.isDirty = false;	// 磁盘数据是干净的
			cache_db.emplace(varName, std::move(v));	// 移动语义，避免拷贝
		}
		rwMutex->unlock();
	}
	return SUCCESS;
}

int Cache::persisVar(const std::string& varName)
{
	return 0;
}

int Cache::persisVar()
{
	return 0;
}

int Cache::reWrite()
{
	return 0;
}
