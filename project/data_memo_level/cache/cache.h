#pragma once
#ifndef _CACHE_H_
#define _CACHE_H_

#include "../data_type.h"
#include "../disk/disk.h"
#include <unordered_map>

class Disk;

class Cache {
	friend class Disk;
private:
	Disk* disk = nullptr;	// 磁盘对象指针（由上层绑定）
	long long memoSize;	// 缓存大小设定值
	long long curSize;	// 当前缓存大小
	std::unordered_map<std::string, Val> cache;      // <变量名，值>

public:
	Cache(long long memoSize);
	void setDisk(Disk* d);	// 绑定磁盘对象

	// 新增数据
	template <typename T>
	int addData(const std::string& varName, const T& entity);

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

