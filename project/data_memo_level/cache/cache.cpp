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

void Cache::selData(const std::string& varName, std::any& res, int& resCode) {
	// 获取读共享锁
	rwMutex->lock_shared();
	// 命中缓存直接返回（缓存数据必定最新，且效率高）
	if (cache_db.contains(varName)) {
		res = cache_db.at(varName).entity;	// 返回值的实体，不是整个 Val
		rwMutex->unlock_shared();
		resCode = SUCCESS;
		return;
	}
	// 未命中：先解锁，再走磁盘查询，避免持锁做磁盘 IO
	rwMutex->unlock_shared();

	// 提交磁盘读任务，只有发起者 get() 阻塞等待结果，其他线程不受影响
	// disk 侧找到变量时填好 res（实体）和 val（时间信息：更新时间/是否永久/过期时间）
	Val val;
	std::future<int> fut = diskThread->submit([this, &varName, &res, &val]() {
		return disk->selData(varName, res, val);
	}, DISK_TASK);	// 读文件，磁盘 IO 任务
	int code = fut.get();
	if (code != SUCCESS) {
		resCode = code;	// 磁盘也找不到
		return;
	}

	// 回填缓存前重新校验 key（可能已被并发修改/删除），校验通过才回填
	// disk 已填时间信息，缓存只补 isDirty 和 entity
	{
		rwMutex->lock();	// 回填是写操作，拿写锁
		if (!cache_db.contains(varName)) {
			val.isDirty = false;	// 磁盘数据是干净的
			val.entity = res;		// 实体从磁盘读回
			cache_db.emplace(varName, std::move(val));	// 移动语义，避免拷贝
		}
		rwMutex->unlock();
	}
	resCode = SUCCESS;
}

std::future<int> Cache::delData(const std::string& varName)
{
	// 锁内只做"查 + 删"，粒度最小
	rwMutex->lock();
	auto it = cache_db.find(varName);
	if (it != cache_db.end()) {
		cache_db.erase(it);	// 命中：从缓存移除
	}
	rwMutex->unlock();

	// 提交磁盘删除任务：磁盘线程检查 inDisk（存在则标记 delDisk），只动内存标记不进文件
	// 任务不引用捕获调用者变量（异步执行时调用者栈帧可能已销毁），结果走 future 通道
	return diskThread->submit([this, varName]() {
		return disk->delData(varName);
	}, CACHE_TASK);
}

std::future<int> Cache::persisVar(const std::string& varName)
{
	/*
		bool varExist = true;
		{
			rwMutex->lock_shared();
			if (!cache_db.contains(varName)) {
				varExist = false;
			}
			rwMutex->unlock_shared();
		}
	// 未找到该变量，无法进行持久化
	if (!varExist) return FIND_FAILED;
	*/

	// 由于考虑到磁盘异步处理的时候，cache_db可能因为并发把数据删了，所以直接给磁盘线程检查，缓存模块不检查，防止磁盘访问不到该数据后崩溃

	// 找到该数据，进行持久化（写文件，磁盘 IO 任务），提交即返回，结果走 future
	return diskThread->submit([this, varName]() { return disk->persisData(varName); }, DISK_TASK);
}

std::future<int> Cache::persisVar()
{
	// 持久化缓存中所有脏数据：磁盘线程执行 persisAll（内部遍历 cache_db 收集脏数据并写文件）
	// 提交即返回，结果走 future
	return diskThread->submit([this]() { return disk->persisAll(); }, DISK_TASK);
}

std::future<int> Cache::reWrite()
{
	// 直接交给disk一起处理，降低业务逻辑耦合，更易维护（读写文件，磁盘 IO 任务）
	// 提交即返回，结果走 future（需要确认重写完成时 get()）
	return diskThread->submit([this]() { return disk->reWrite(); }, DISK_TASK);
}
