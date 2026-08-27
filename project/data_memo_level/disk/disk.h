#pragma once
#ifndef _DISK_H_
#define _DISK_H_

#include "../data_type.h"
#include "../cache/cache.h"
#include <list>
#include <unordered_map>

class Cache;

class Disk {
	friend class Cache;
private:
	Cache* cache = nullptr;	// 缓存对象指针（由上层绑定）
	long long maxSize;		// 磁盘最大容量
	long long curSize;		// 磁盘当前容量
	std::string dbName;		// 数据库名称(文件名)
	std::unordered_map<std::string, std::pair<int, int>> inDisk;		// 变量名 + 偏移量 (数据存储first + 时间存储second)
	std::unordered_set<std::string> delDisk;			// 待删除的变量名

public:
	explicit Disk(const long long& maxSize, std::string& dbName);

	/* ========== special operation 特殊操作 ========== */

	inline bool containsVar(std::string varName) { return inDisk.contains(varName) && !delDisk.contains(varName); }

	/* ========== operation function(操作函数) ========== */

	// 绑定缓存对象
	void setCache(Cache* c);	
	// 删除某变量
	int delData(const std::string& varName);
	// 持久化某变量
	int persisData(const std::string& varName);
	// 持久化某变量的所有数据
	int persisData(std::list<std::string>& varNameSet);
	// 持久化所有数据
	int persisAll();
	// 查询某变量
	int selData(const std::string& varName, std::any& res, Val& val);
	// 查询多个变量
	int selData(std::list<std::string>& varNameSet, std::list<std::any>& resSet);
	// 刷盘
	int flushDisk();
	// 数据重写：缓存+inDisk 数据写入新文件，删旧文件，更新 inDisk，清空 delDisk
	int reWrite();	

};

#endif // !_DISK_H_
