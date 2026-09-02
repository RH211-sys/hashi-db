#pragma once
#ifndef _CONTROLLER_H_
#define _CONTROLLER_H_

#include "../cache/cache.h"
#include "../disk/disk.h"
#include "taskThreads.h"
#include <memory>

/*
	Controller：对上层的统一封装入口
	- 组装 Cache / Disk / 线程池并注入依赖（setDisk / setCache / setDiskThread）
	- CRUD 与持久化 API 全部同步返回错误码，上层无需关心内部线程调度
	- 读并发由缓存模块的读写锁保证；磁盘 IO 统一走磁盘线程
*/
class Controller {
private:
	std::shared_ptr<Cache> cache;				// 缓存对象（shared_ptr：Cache 内部持有 Disk/DiskThread 的 shared_ptr，需共享所有权）
	std::shared_ptr<Disk> disk;					// 磁盘对象
	std::shared_ptr<ReadPool> readPool;			// 读并发任务入口（暂未启用，预留上层并发调度）
	std::shared_ptr<WriteThread> writeThread;	// 写任务（Cache 持有其 shared_ptr，需共享所有权）
	std::shared_ptr<DiskThread> diskThread;		// 磁盘任务线程（Cache 持有其 shared_ptr，需共享所有权）

public:
	/*
		功能：创建数据库实例，组装缓存、磁盘与线程池，注入相互依赖
		参数：memoSize：缓存大小设定值（缓存占用超过后触发 LRU 淘汰）
		      maxSize：磁盘最大容量
		      poolThreadNum：读线程池线程数（预留，暂未启用）
		      dbName：数据库文件名（数据落盘文件）
		返回值：无
	*/
	Controller(long long memoSize, long long maxSize, size_t poolThreadNum, std::string dbName);

	/*
		功能：新增一条数据（插入缓存并异步刷盘；缓存已存在或磁盘已存在均视为冲突）
		参数：varName：变量名
		      entity：值实体（类型 T 需实现 getClassName / serialize / theSize）
		      isPermanent：是否永不过期
		      during：数据持续时间（isPermanent 为 false 时生效，过期时间 = 更新时间 + during）
		返回值：错误码，SUCCESS 成功 / KEY_EXIST 变量已存在
	*/
	template <typename T>
	int addData(const std::string& varName, T entity, bool isPermanent, std::chrono::system_clock::duration during) {
		int resCode;
		cache->addData(varName, std::move(entity), isPermanent, during, resCode);
		return resCode;
	}

	/*
		功能：整体修改某变量（命中缓存直接替换实体并标脏；缓存未命中但磁盘存在则回填缓存并标脏）
		参数：varName：变量名
		      entity：新的值实体（类型 T 需实现 getClassName / serialize / theSize）
		返回值：错误码，SUCCESS 成功 / FIND_FAILED 变量不存在
	*/
	template <typename T>
	int modData(const std::string& varName, T entity) {
		int resCode;
		cache->modData(varName, std::move(entity), resCode);
		return resCode;
	}

	/*
		功能：删除某变量（缓存删除 + 磁盘索引删除，同步等待磁盘任务完成）
		参数：varName：变量名
		返回值：错误码，SUCCESS 成功 / FIND_FAILED 变量不存在
	*/
	int delData(const std::string& varName);

	/*
		功能：查询某变量（命中缓存直接返回；未命中走磁盘读取并回填缓存；过期数据舍弃并提交磁盘删除）
		参数：varName：变量名
		      res：输出参数，查询结果实体（查不到或已过期时不填）
		返回值：错误码，SUCCESS 成功 / FIND_FAILED 不存在 / EXPIRED 已过期
	*/
	int selData(const std::string& varName, std::any& res);

	/*
		功能：单个变量持久化刷盘，同步等待磁盘结果
		参数：varName：变量名
		返回值：错误码，SUCCESS 成功
	*/
	int persisVar(const std::string& varName);

	/*
		功能：持久化缓存中所有脏数据，同步等待磁盘结果
		参数：无
		返回值：错误码，SUCCESS 成功
	*/
	int persisAll();

	/*
		功能：刷盘（清理过期数据 + 落盘所有脏数据）
		参数：无
		返回值：错误码，SUCCESS 成功
	*/
	int flushDisk();

	/*
		功能：数据重写（缓存与磁盘数据写入新文件，回收空洞并删除旧文件）
		参数：无
		返回值：错误码，SUCCESS 成功
	*/
	int reWrite();
};

#endif
