#pragma once
#ifndef _DISK_H_
#define _DISK_H_

#include "../data_type.h"
#include <fstream>
#include <utility>
#include <vector>
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

	// ===== 磁盘 IO（仅磁盘线程访问，天然串行，无需锁）=====
	std::fstream file;			// 常驻文件句柄（读+写：所有持久化/重写复用，免每次 open/close）
	std::vector<char> recBuf;	// 单条记录组装缓冲（定长头 + 实体字节，序列化直写、整条一次落盘）

	// 惰性打开/IO 错误后重建句柄：确保 file 已打开且可用（仅在写路径调用）
	bool ensureFileOpen();
	// 组装单条记录到 recBuf：定长头（校验码由 code 参数给定）+ 实体字节（直接序列化进缓冲）
	// 成功返回 SUCCESS 并写出 dataSize（实体字节数）/ recLen（整条长度，= recBuf.size()）
	int buildRecord(const std::string& varName, const Val& val, char code, int& dataSize);
	// 追加写单条记录（两段式校验码：先整条 CHECK_BROKEN，写毕回写 CHECK_VALID）
	// 不 flush（调用方按批 flush）；成功更新 inDisk/curSize
	int appendRecord(const std::string& varName, const Val& val);

public:
	explicit Disk(const long long& maxSize, std::string& dbName);
	~Disk();

	/* ========== special operation 特殊操作 ========== */

	inline bool containsVar(const std::string& varName) { return inDisk.contains(varName); }

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
	// 持久化变量集（带数据批量版：一次文件开关写整批，数据由调用方提供，供淘汰攒批刷盘用）
	int persisData(std::vector<std::pair<std::string, Val>> dataSet);
	// 持久化某变量集
	int persisData(std::vector<std::string> varNameSet);
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
