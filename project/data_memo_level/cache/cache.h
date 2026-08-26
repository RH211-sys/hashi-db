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

	// 新增数据
	template <typename T>
	int addData(const std::string& varName, const T& entity) {
		
	}

	// 删除数据
	int delData(const std::string& varName);

	// 修改整个变量
	template <typename T>
	int modData(const std::string& varName, const T& entity);
	// 用于修改某个变量的某个属性
	template <typename T>
	int modData(const std::string& varName, std::string& member, const T& entity);

	// 查找，结果写入 res
	int selData(const std::string& varName, std::any& res);

	// 单个变量持久化
	int persisVar(const std::string& varName);
	// 缓存全部持久化
	int persisVar();


	// 数据重写：将缓存和inDisk中的所有数据写入到另一个文件中，并删除旧文件
	int reWrite();



};

#endif // !_CACHE_H_

