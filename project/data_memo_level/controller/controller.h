#pragma once
#ifndef _CONTROLLER_H_
#define _CONTROLLER_H_

#include "../cache/cache.h"
#include "../disk/disk.h"
#include "taskThreads.h"
#include <memory>

/*
	Controller：对上层的统一封装入口
	- 组装 Cache / Disk / 线程池并注入依赖（bind / setCache）
	- 存储层全部后台异步执行，CRUD 与持久化 API 返回 future，上层需要结果时 get() 阻塞等待
	- 读并发由缓存模块的读写锁保证；磁盘 IO 统一走磁盘线程
*/
class Controller {
private:
	std::shared_ptr<Cache> cache;				// 缓存对象（shared_ptr：Cache 内部持有 Disk/DiskThread 的 shared_ptr，需共享所有权）
	std::shared_ptr<Disk> disk;					// 磁盘对象
	std::shared_ptr<ReadPool> readPool;			// 读线程池：缓存读任务并发执行（Cache 持有其 shared_ptr，需共享所有权）
	std::shared_ptr<WriteThread> writeThread;	// 写线程：缓存写任务串行执行（Cache 持有其 shared_ptr，需共享所有权）
	std::shared_ptr<DiskThread> diskThread;		// 磁盘线程：磁盘索引与文件 IO 任务（Cache 持有其 shared_ptr，需共享所有权）

public:
	/*
		功能：创建数据库实例，组装缓存、磁盘与线程池，注入相互依赖
		参数：memoSize：缓存大小设定值（缓存占用超过后触发全局采样淘汰）
		      maxSize：磁盘最大容量
		      poolThreadNum：读线程池线程数（并发执行缓存读任务）
		      dbName：数据库文件名（数据落盘文件）
		返回值：无
	*/
	Controller(long long memoSize, long long maxSize, size_t poolThreadNum, std::string dbName);

	/*
		功能：新增一条数据（异步执行：插入缓存并刷盘；缓存已存在或磁盘已存在均视为冲突）
		参数：varName：变量名
		      entity：值实体（类型 T 需实现 getClassName / serialize / theSize）
		      isPermanent：是否永不过期
		      during：数据持续时间（isPermanent 为 false 时生效，过期时间 = 更新时间 + during）
		返回值：std::future<int>，get() 后取错误码：SUCCESS 成功 / KEY_EXIST 变量已存在
	*/
	template <typename T>
	std::future<int> addData(const std::string& varName, T entity, bool isPermanent, std::chrono::system_clock::duration during) {
		// 写任务已在缓存模块内交写线程执行，直接透传 future，调用方不阻塞
		return cache->addData(varName, std::move(entity), isPermanent, during);
	}

	/*
		功能：整体修改某变量（异步执行：命中缓存直接替换实体并标脏；缓存未命中但磁盘存在则回填缓存并标脏）
		参数：varName：变量名
		      entity：新的值实体（类型 T 需实现 getClassName / serialize / theSize）
		返回值：std::future<int>，get() 后取错误码：SUCCESS 成功 / FIND_FAILED 变量不存在
	*/
	template <typename T>
	std::future<int> modData(const std::string& varName, T entity) {
		// 写任务已在缓存模块内交写线程执行，直接透传 future，调用方不阻塞
		return cache->modData(varName, std::move(entity));
	}

	/*
		功能：删除某变量（异步执行：缓存删除 + 磁盘索引删除）
		参数：varName：变量名
		返回值：std::future<int>，get() 后取错误码：SUCCESS 成功 / FIND_FAILED 变量不存在
	*/
	std::future<int> delData(const std::string& varName);

	/*
		功能：查询某变量（异步执行：命中缓存直接返回实体指针；未命中走磁盘读取并回填缓存；过期数据舍弃并提交磁盘删除）
		参数：varName：变量名
		返回值：std::future<SelResult>，get() 后取 resCode（SUCCESS 成功 / FIND_FAILED 不存在 / EXPIRED 已过期）
		       与 entity（实体指针：与缓存共享同一实体，零拷贝；只读约定，修改请走 modData）
	*/
	std::future<SelResult> selData(const std::string& varName);

	/*
		功能：单个变量持久化刷盘（异步执行，不阻塞调用方）
		参数：varName：变量名
		返回值：std::future<int>，get() 后取错误码：SUCCESS 成功
	*/
	std::future<int> persisVar(const std::string& varName);

	/*
		功能：持久化缓存中所有脏数据（异步执行，不阻塞调用方）
		参数：无
		返回值：std::future<int>，get() 后取错误码：SUCCESS 成功
	*/
	std::future<int> persisAll();

	/*
		功能：刷盘（清理过期数据 + 落盘所有脏数据）（异步执行，不阻塞调用方）
		参数：无
		返回值：std::future<int>，get() 后取错误码：SUCCESS 成功
	*/
	std::future<int> flushDisk();

	/*
		功能：数据重写（缓存与磁盘数据整理写入，回收空洞并删除旧文件）（异步执行，不阻塞调用方）
		参数：无
		返回值：std::future<int>，get() 后取错误码：SUCCESS 成功
	*/
	std::future<int> reWrite();

	/*
		功能：读取缓存运行统计快照（命中/未命中/淘汰计数与耗时，供性能测试与运行观测）
		参数：无
		返回值：CacheStat（各字段含义见 cache.h 顶部结构体定义）
	*/
	CacheStat getStat() const;

	/*
		功能：读取磁盘 IO 阶段统计（组装序列化 / 文件写 / flush / 磁盘读，供性能归因）
		参数：无
		返回值：DiskIoStat（字段含义见 disk.h 结构体定义）
	*/
	DiskIoStat getIoStat() const;

	/*
		功能：读取磁盘线程队列统计（提交量 / 队列积压 / worker 忙碌，供性能归因）
		参数：无
		返回值：DiskQueueStat（字段含义见 taskThreads.h 结构体定义）
	*/
	DiskQueueStat getQueueStat() const;
};

#endif
