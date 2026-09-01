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
	std::weak_ptr<Cache> cache;	// 缓存对象指针，用weak防止内存泄露
	long long maxSize;		// 磁盘最大容量
	long long curSize;		// 磁盘当前容量
	std::string dbName;		// 数据库名称(文件名)
	std::unordered_map<std::string, int> inDisk;		// 变量名 + 偏移量

public:
	explicit Disk(const long long& maxSize, std::string& dbName);

	/* ========== special operation 特殊操作 ========== */

	inline bool containsVar(std::string varName) { return inDisk.contains(varName); }

	/* ========== operation function(操作函数) ========== */

	// 绑定缓存对象
	void setCache(std::shared_ptr<Cache> c);
	// 删除某变量
	int delData(const std::string& varName);
	// 批量删除变量（集合版，一次提交）
	int delData(std::vector<std::string> varNameSet);
	// 持久化某变量
	int persisData(const std::string& varName);
	// 持久化某变量（带数据版：数据由调用方提供，不查缓存，供淘汰刷盘用）
	int persisData(const std::string& varName, const Val& val);
	// 持久化某变量集
	int persisData(std::vector<std::string>& varNameSet);
	// 持久化某个数据
	
	// 持久化所有数据
	int persisAll();
	// 查询某变量
	int selData(const std::string& varName, std::any& res, Val& val);
	// 查询多个变量
	int selData(std::vector<std::string>& varNameSet, std::vector<std::any>& resSet, std::vector<Val>& vals);
	// 刷盘
	int flushDisk();
	// 数据重写：缓存+inDisk 数据写入新文件，删旧文件，更新 inDisk，清空 delDisk
	int reWrite();	

};

#endif // !_DISK_H_
