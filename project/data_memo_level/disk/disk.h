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
	std::unordered_map<std::string, int> inDisk;		// 变量名 + 偏移量
	std::unordered_set<std::string> delDisk;			// 待删除的变量名

public:
	explicit Disk(const long long& maxSize, std::string& dbName);
	void setCache(Cache* c);	// 绑定缓存对象
	// 删除某变量
	int delData(const std::string& varName);
	// 持久化某变量
	int persisData(const std::string& varName);
	// 持久化某变量的所有数据
	int persisData(std::list<std::string>& varNameSet);
	// 查询某变量
	int selData(const std::string& varName, std::any& res);
	// 查询多个变量
	int selData(std::list<std::string>& varNameSet, std::list<std::any>& resSet);
	// 刷盘
	int flushDisk();

};

#endif // !_DISK_H_
