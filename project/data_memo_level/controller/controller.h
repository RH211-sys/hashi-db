#pragma once
#ifndef _CONTROLLER_H_
#define _CONTROLLER_H_

#include "../cache/cache.h"
#include "../disk/disk.h"
#include "taskThreads.h"
#include <memory>

class Controller {
private:
	std::unique_ptr<Cache> cache;				// 持有缓存对象
	std::unique_ptr<Disk> disk;					// 持有磁盘对象
	std::unique_ptr<ReadPool> readPool;			// 读并发任务入口
	std::unique_ptr<WriteThread> writeThread;	// 写任务
	std::unique_ptr<DiskThread> diskThread;		// 磁盘任务

public:
	Controller(long long memoSize, long long maxSize, size_t poolThreadNum);

	// ---- CRUD API ----
	template <typename T>
	int addData(const std::string& varName, const T& entity) { return cache->addData(varName, entity); }

	int delData(const std::string& varName);

	template <typename T>
	int modData(const std::string& varName, const T& entity) { return cache->modData(varName, entity); }

	template <typename T>
	int modData(const std::string& varName, const std::string& member, const T& entity) { return cache->modData(varName, member, entity); }

	int selData(const std::string& varName, std::any& res);

	// ---- 持久化 API ----
	int persisVar(const std::string& varName);	// 提交任务,不阻塞
	int flushDisk();		// 刷盘
	int reWrite();			// 重写
};

#endif