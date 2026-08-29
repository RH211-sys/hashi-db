#pragma once
#ifndef _CACHE_H_
#define _CACHE_H_

#include "../data_type.h"
#include "../disk/disk.h"
#include "../common/writePrefMutex.h"
#include "../controller/taskThreads.h"
#include <memory>
#include <unordered_map>

class Disk;

class Cache {
	friend class Disk;
private:
	std::shared_ptr<Disk> disk;				// 磁盘对象（由上层注入，共享所有权）
	std::shared_ptr<DiskThread> diskThread;	// 磁盘线程（由上层注入，共享所有权），用于异步提交磁盘读任务
	long long memoSize;	// 缓存大小设定值
	long long curSize;	// 当前缓存大小
	std::unordered_map<std::string, Val> cache_db;      // <变量名，值>
	std::unique_ptr<WritePrefMutex> rwMutex;			// 缓存模块的写优先读写锁



public:
	Cache(const long long memoSize, const int batchSize = 4, const int upDisEdge = 10, const int minDisEdge = -3);
	void setDisk(std::shared_ptr<Disk> d);	// 绑定磁盘对象
	void setDiskThread(std::shared_ptr<DiskThread> t);	// 绑定磁盘线程对象

	/*  功能：新增数据，
		参数：变量名，值实体，是否永久，数据持续时间（过期时间 = 更新时间 + 持续时间）
		resCode：操作码（eg：200，表示操作成功）
	*/
	template <typename T>
	void addData(const std::string& varName, const T& entity, bool isPermanent, std::chrono::system_clock::duration during, int& resCode) {
		// 锁外构造 Val（本地操作不需要锁）
		Val v;
		v.typeName = T::getClassName();	// 类型名称
		v.isPermanent = isPermanent;	// 是否永不过期
		v.updateTime = std::chrono::system_clock::now();
		if (!isPermanent) {
			v.expireTime = v.updateTime + during;	// 非永久：过期时间 = 更新时间 + 持续时间
		}
		v.isDirty = true;				// 新数据标记为脏，等待刷盘
		v.entity = entity;

		// 锁内只做"检查"，锁的粒度最小：缓存已有则冲突
		rwMutex->lock();
		if (cache_db.contains(varName)) {
			rwMutex->unlock();
			resCode = KEY_EXIST;
			return;
		}
		rwMutex->unlock();

		// 缓存没有：查磁盘是否已存在（走磁盘线程查 inDisk 内存索引，微秒级）
		// 磁盘已有同样算冲突，不允许重复新增
		std::future<bool> fut = diskThread->submit([this, varName]() {
			return disk->containsVar(varName);
		}, CACHE_TASK);
		if (fut.get()) {
			resCode = KEY_EXIST;	// 磁盘已存在该数据
			return;
		}

		// 校验通过：重新加锁插入（并发期间可能已有其他线程插入，需重新校验 key）
		rwMutex->lock();
		if (!cache_db.contains(varName)) {
			cache_db.emplace(varName, std::move(v));	// 移动语义，避免拷贝
		}
		rwMutex->unlock();
		resCode = SUCCESS;
	}

	// 删除数据：提交即返回，不阻塞；需要准确结果时对返回的 future 调用 get()
	std::future<int> delData(const std::string& varName);

	// 修改某个变量，整体修改
	template <typename T>
	void modData(const std::string& varName, const T& entity, int& resCode) {
		// 锁内只做"查 + 替换"
		rwMutex->lock();
		auto it = cache_db.find(varName);
		if (it != cache_db.end()) {
			it->second.entity = entity;	// 命中：替换实体值
			it->second.updateTime = std::chrono::system_clock::now();	// 更新时间
			it->second.isDirty = true;	// 标记为脏数据，等待刷盘
			rwMutex->unlock();
			resCode = SUCCESS;
			return;
		}
		rwMutex->unlock();
		// 缓存未命中：查磁盘是否存在（inDisk 有且未删 → 允许新增缓存标脏，刷盘时更新磁盘）
		// 走磁盘线程查 inDisk（内存索引，微秒级），避免缓存线程直接访问磁盘状态
		std::future<bool> fut = diskThread->submit([this, varName]() {
			return disk->containsVar(varName);
		}, CACHE_TASK);	// 查 inDisk 内存索引，缓存任务
		if (!fut.get()) {
			resCode = FIND_FAILED;	// 磁盘也没有，无法修改
			return;
		}

		// 校验通过：新增缓存并标脏（重新校验 key，防止并发已插入），锁前构造val
		Val v;
		v.typeName = T::getClassName();
		v.isPermanent = true;	// 磁盘数据不存过期信息，默认永久
		v.updateTime = std::chrono::system_clock::now();
		v.isDirty = true;		// 脏数据，等待刷盘更新磁盘
		v.entity = entity;
		rwMutex->lock();
		if (!cache_db.contains(varName)) {
			cache_db.emplace(varName, std::move(v));
		}
		rwMutex->unlock();
		resCode = SUCCESS;
	}
	// 用于修改某个变量的某个属性
	template <typename T>
	void modData(const std::string& varName, std::string& member, const T& entity, int& resCode);

	// 查找，结果写入 res
	void selData(const std::string& varName, std::any& res, int& resCode);

	// 单个变量持久化：提交即返回，不阻塞，结果走 future
	std::future<int> persisVar(const std::string& varName);
	// 缓存全部持久化：提交即返回，不阻塞，结果走 future
	std::future<int> persisVar();


	// 数据重写：将缓存和inDisk中的所有数据写入到另一个文件中，并删除旧文件
	std::future<int> reWrite();



};

#endif // !_CACHE_H_

