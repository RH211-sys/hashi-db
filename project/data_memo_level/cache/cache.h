#pragma once
#ifndef _CACHE_H_
#define _CACHE_H_

#include "../data_type.h"
#include "../disk/disk.h"
#include "../common/writePrefMutex.h"
#include "../controller/taskThreads.h"
#include <atomic>
#include <memory>
#include <unordered_map>
#include <cmath>	// 用于计算ln
#include <future>		// promise/future（AsyncResult 续延桶）
#include <exception>	// current_exception（AsyncResult 异常兜底）

/*
	缓存运行统计快照（供性能测试与运行观测）：命中/未命中/淘汰计数与耗时
	计数为原子累加值，读取瞬间的一致性要求不高（观测用途）
	命中/未命中在热路径 relaxed 自增；evict 只在写线程执行，原子累加无争抢
*/
struct CacheStat {
	long long hit = 0;			// 命中次数：缓存中存在即算命中（含已过期——过期是 TTL 语义，不算缓存未命中）
	long long miss = 0;			// 未命中次数：缓存中不存在转入磁盘读取（磁盘读回成功与否都算缓存未命中）
	long long evictCnt = 0;		// 淘汰执行次数
	long long evictItems = 0;	// 淘汰条目数（过期批量清理 + 采样淘汰）
	long long evictUs = 0;		// 淘汰总耗时（微秒）
};

/*
	异步结果桶（AsyncResult）：缓存接口在"服务线程不阻塞等待"的形态下跨线程兑现请求结果
	背景——任务内同步 get() 等磁盘线程会把单线程执行者（写线程）或有限线程池（读池）占死：
	磁盘线程执行慢任务（文件 IO，毫秒级）期间，其后排队的任务即使只需微秒也要等它跑完，
	等待的执行者因此被慢 IO 无辜拖累，吞吐崩塌、尾部暴涨；
	正确形态是"段只投递不等待"：一个请求分多段在不同线程依次投递，结果最终经 promise 兑现，
	真正需要结果的人（调用方 get()）阻塞，中间服务线程永远不等 IO。
	封装点：
	- promise 生命周期：shared_ptr 跨段共享，各段闭包按值捕获同一桶（桶可拷贝）
	- 段体异常兜底 run()：各线程池 work() 会吞异常，漏记会让调用方 get() 永久等待 → 统一转 set_exception
	- 线程停止兜底 fallback()：后续段被拒收（线程池已停止，submit 返回空 future）→ 兑现兜底值
*/
template <typename T>
class AsyncResult {
public:
	// stopVal：线程已停止、段无法投递时的兜底结果（避免调用方永久等待）
	explicit AsyncResult(T stopVal) : stopVal(std::move(stopVal)), prom(std::make_shared<std::promise<T>>()) {}
	std::future<T> future() { return prom->get_future(); }	// 给调用方等待的 future
	// 执行一段任务体：内部异常统一转 set_exception（每请求只兑现一次；done 之后无抛点）
	template <typename F>
	void run(F&& seg) {
		try { seg(); }
		catch (...) { prom->set_exception(std::current_exception()); }
	}
	// 兑现最终结果（错误码也走这里；每个请求恰好一次）
	void done(T v) { prom->set_value(std::move(v)); }
	// 线程已停止、段被拒收时的兜底兑现
	void fallback() { done(stopVal); }
private:
	T stopVal;									// 兜底结果
	std::shared_ptr<std::promise<T>> prom;		// 结果桶：跨段共享，全部段结束时由调用方 future 消费
};

/*
	宏接口：用于调用该类型的静态成员函数，计算对象大小
	参数：type数据类型，name对象名
*/
#define THE_SIZE(type, name) type::theSize(name)   // 用作表达式

class Disk;

class Cache {
	friend class Disk;
private:
	std::shared_ptr<Disk> disk;				// 磁盘对象（由上层注入，共享所有权）
	std::shared_ptr<DiskThread> diskThread;	// 磁盘线程（由上层注入，共享所有权），用于异步提交磁盘读任务
	std::shared_ptr<WriteThread> writeThread;	// 写线程（由上层注入，共享所有权），缓存写任务串行执行
	std::shared_ptr<ReadPool> readPool;		// 读线程池（由上层注入，共享所有权），缓存读任务并发执行
	long long memoSize;	// 缓存大小设定值
	long long curSize;	// 当前缓存大小
	std::unordered_map<std::string, Val> cache_db;      // <变量名，值>
	std::unique_ptr<WritePrefMutex> rwMutex;			// 缓存模块的写优先读写锁
	// 运行统计（原子计数，供 getStat() 观测；命中/未命中在热路径 relaxed 自增，evict 只在写线程执行无争抢）
	std::atomic<long long> statHit{ 0 };		// 命中次数（缓存中存在即命中，含过期）
	std::atomic<long long> statMiss{ 0 };		// 未命中次数（转入磁盘读取）
	std::atomic<long long> statEvictCnt{ 0 };	// 淘汰执行次数
	std::atomic<long long> statEvictItems{ 0 };	// 淘汰条目数（过期清理 + 采样淘汰）
	std::atomic<long long> statEvictUs{ 0 };	// 淘汰总耗时（微秒）
private:
	// 淘汰分数计算：距上次读命中/插入的档位 × 档距 + ln(大小)，分数越高越优先淘汰
	double evictScore(Val& val);
	// 全局采样淘汰（无 LRU 链表）：随机桶采样(unordered_map的桶) + 优先队列，阻塞执行到容量达标（写线程内调用）
	void evict();
public:
	Cache(const long long memoSize, const int batchSize = 4, const int upDisEdge = 10, const int minDisEdge = -3);

	/*
		功能：统一绑定依赖对象：磁盘线程、磁盘对象、读线程池（由上层 Controller 组装时注入）
	*/
	void bind(std::shared_ptr<DiskThread> diskThread, std::shared_ptr<Disk> d, std::shared_ptr<ReadPool> readPool, std::shared_ptr<WriteThread> writeThread);

	/*
		功能：新增一条数据（写任务在写线程串行执行：共享锁探测缓存 → 磁盘索引查重 → 独占锁插入）
		参数：varName：变量名
		      entity：值实体（类型 T 需实现 getClassName / serialize / theSize）
		      isPermanent：是否永不过期
		      during：数据持续时间（isPermanent 为 false 时生效，过期时间 = 更新时间 + during）
		返回值：std::future<int>，get() 后取错误码：SUCCESS 成功 / KEY_EXIST 变量已存在
	*/
	template <typename T>
	std::future<int> addData(const std::string& varName, T entity, bool isPermanent, std::chrono::system_clock::duration during) {
		// 续延链（写线程不被磁盘同步等待占死）：段1（写线程）缓存查重 → 段2（磁盘线程）索引查重 →
		// 段3（写线程）独占锁插入；只有调用方等结果，服务线程只投递不等待
		AsyncResult<int> ar{ UNKNOWN_ERROR };	// 兜底：线程已停止/段被拒收时返回未知错误
		auto fut = ar.future();
		auto submitOk = writeThread->submit([this, varName, entity = std::move(entity), isPermanent, during, ar]() mutable {
			ar.run([&]() {
				// 1. 共享锁探测缓存（读探测不阻塞并发读）：已存在则冲突
				{
					rwMutex->lock_shared();
					bool exists = cache_db.contains(varName);
					rwMutex->unlock_shared();
					if (exists) { ar.done(KEY_EXIST); return; }
				}

				// 2. 锁外构造 Val（实体移进堆，零拷贝）
				Val v;
				v.typeName = T::getClassName();	// 类型名称
				v.isPermanent = isPermanent;	// 是否永不过期
				long long nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::system_clock::now().time_since_epoch()).count();	// µs since epoch
				v.updateTime.store(nowUs, std::memory_order_relaxed);	// 插入即"最近访问"= now
				if (!isPermanent) {
					v.expireTime = std::chrono::system_clock::time_point(std::chrono::microseconds(nowUs)) + during;	// 非永久：过期时间 = 更新时间 + 持续时间
				}
				v.isDirty = true;				// 新数据标记为脏，等待刷盘
				v.dataSize = THE_SIZE(T, entity);	// 数据大小（用户自定义 theSize 计算）
				v.entity = std::make_shared<std::any>(std::move(entity));	// 实体移动进堆，避免拷贝

				// 3. 段2（磁盘线程）：磁盘索引查重（内存任务微秒级）：磁盘已有同样算冲突，不允许重复新增
				auto ioFut = diskThread->submit([this, varName, v = std::move(v), ar]() mutable {
					ar.run([&]() {
						if (disk->containsVar(varName)) { ar.done(KEY_EXIST); return; }	// 磁盘已存在该数据
						// 4. 段3（写线程）：独占锁插入（锁内重新校验 key：等待期间可能已有并发插入）
						auto seg3 = writeThread->submit([this, varName, v = std::move(v), ar]() mutable {
							ar.run([&]() {
								long long sizeNow = 0;
								{
									rwMutex->lock();
									if (!cache_db.contains(varName)) {
										cache_db.emplace(varName, std::move(v));	// 移动语义，避免拷贝
										curSize += v.dataSize;			// 更新缓存当前大小
									}
									sizeNow = curSize;	// 锁内快照：解锁后读它判断是否触发淘汰，避免锁外读竞态
									rwMutex->unlock();
								}
								// 5. 超过设定容量：锁外触发淘汰（全局采样，写线程内阻塞执行，防刚插入就超限）
								if (sizeNow > memoSize) evict();
								ar.done(SUCCESS);
							});
						});
						if (!seg3.valid()) ar.fallback();	// 写线程已停止：段3 被拒收
					});
				}, CACHE_TASK);
				if (!ioFut.valid()) ar.fallback();	// 磁盘线程已停止：段2 被拒收
			});
		});
		if (!submitOk.valid()) ar.fallback();	// 写线程已停止：段1 被拒收
		return fut;
	}

	/*
		功能：删除某变量（缓存删除 + 磁盘索引删除，整链在写线程执行）
		参数：varName：变量名
		返回值：std::future<int>，get() 后取错误码：SUCCESS 成功 / FIND_FAILED 变量不存在
	*/
	std::future<int> delData(const std::string& varName);

	/*
		功能：整体修改某变量（写任务在写线程串行执行：独占锁内命中即替换；未命中查磁盘索引，存在则回填标脏）
		参数：varName：变量名
		      entity：新的值实体（类型 T 需实现 getClassName / serialize / theSize）
		返回值：std::future<int>，get() 后取错误码：SUCCESS 成功 / FIND_FAILED 变量不存在
	*/
	template <typename T>
	std::future<int> modData(const std::string& varName, T entity) {
		// 续延链（写线程不被磁盘同步等待占死）：段1（写线程）锁内查缓存命中即替换，未命中只提交磁盘查索引；
		// 段2（磁盘线程）查索引，存在则投递段3（写线程）回填标脏；只有调用方等结果，服务线程只投递不等待
		AsyncResult<int> ar{ UNKNOWN_ERROR };	// 兜底：线程已停止/段被拒收时返回未知错误
		auto fut = ar.future();
		auto submitOk = writeThread->submit([this, varName, entity = std::move(entity), ar]() mutable {
			ar.run([&]() {
				// 0. 锁外算好新实体大小与实体指针（锁内只做记账与指针交换，临界区短）
				long long newSize = THE_SIZE(T, entity);	// 新实体大小
				auto newEntity = std::make_shared<std::any>(std::move(entity));	// 实体移进堆一次

				// 1. 独占锁内查缓存：命中即替换（替换 shared_ptr 指针，不碰实体内容；旧实体移出，锁外析构）
				{
					rwMutex->lock();
					auto it = cache_db.find(varName);
					if (it != cache_db.end()) {
						curSize = curSize - it->second.dataSize + newSize;	// 先减旧再加新
						auto oldEntity = std::move(it->second.entity);	// 移出旧实体，锁内只做指针交换，析构发生在解锁后
						it->second.entity = newEntity;	// 放入新实体
						it->second.dataSize = newSize;
						// it->second.updateTime = std::chrono::system_clock::now();	// 不更新时间，因为写的可能是冷数据
						it->second.isDirty = true;	// 标记为脏数据，等待刷盘
						long long sizeNow = curSize;	// 锁内快照：解锁后判断是否触发淘汰
						rwMutex->unlock();
						// 实体变大可能超限：锁外触发淘汰
						if (sizeNow > memoSize) evict();
						ar.done(SUCCESS);
						return;
					}
					rwMutex->unlock();
				}

				// 2. 缓存未命中：段2（磁盘线程）查磁盘索引（inDisk 有且未删 → 允许回填缓存标脏，刷盘时更新磁盘）
				auto ioFut = diskThread->submit([this, varName, newSize, newEntity, ar]() mutable {
					ar.run([&]() {
						if (!disk->containsVar(varName)) { ar.done(FIND_FAILED); return; }	// 磁盘也没有，无法修改
						// 3. 段3（写线程）：磁盘存在 → 锁外构造 Val（磁盘数据不存过期信息，默认永久），独占锁内回填
						Val v;
						v.typeName = T::getClassName();
						v.isPermanent = true;	// 磁盘数据不存过期信息，默认永久
						// v.updateTime = std::chrono::system_clock::now(); 
						v.isDirty = true;		// 脏数据，等待刷盘更新磁盘
						v.dataSize = newSize;	// 数据大小
						v.entity = newEntity;	// 与本次修改同一实体指针
						auto seg3 = writeThread->submit([this, varName, v = std::move(v), ar]() mutable {
							ar.run([&]() {
								long long sizeNow = 0;
								{
									rwMutex->lock();
									if (!cache_db.contains(varName)) {	// 锁内重校验：等待期间可能已并发插入
										cache_db.emplace(varName, std::move(v));
										curSize += v.dataSize;			// 更新缓存当前大小
									}
									sizeNow = curSize;	// 锁内快照：解锁后判断是否触发淘汰
									rwMutex->unlock();
								}
								// 回填会增大缓存：超限触发淘汰
								if (sizeNow > memoSize) evict();
								ar.done(SUCCESS);
							});
						});
						if (!seg3.valid()) ar.fallback();	// 写线程已停止：段3 被拒收
					});
				}, CACHE_TASK);	// 查 inDisk 内存索引，缓存任务
				if (!ioFut.valid()) ar.fallback();	// 磁盘线程已停止：段2 被拒收
			});
		});
		if (!submitOk.valid()) ar.fallback();	// 写线程已停止：段1 被拒收
		return fut;
	}

	// 用于修改某个变量的某个属性
	template <typename T>
	void modData(const std::string& varName, std::string& member, const T& entity, int& resCode);

	/*
		功能：查询某变量（异步，提交即返回；命中缓存直接返回实体指针；未命中走磁盘读取并回填缓存；过期数据舍弃并提交磁盘删除）
		参数：varName：变量名
		返回值：std::future<SelResult>，get() 后取 resCode（SUCCESS 成功 / FIND_FAILED 不存在 / EXPIRED 已过期）
		       与 entity（实体指针：与缓存共享同一实体，零拷贝；失败时为空）
		说明：查询链在读线程池执行（读锁并发查询，命中纯读零写）；命中过期只提交清理任务不等待；
		      未命中在磁盘线程内完成"读盘 + 过期判定 + 过期直删"，回填交写线程后台处理（实体指针保活）
	*/
	std::future<SelResult> selData(const std::string& varName);

	// 单个变量持久化：提交即返回，不阻塞，结果走 future
	std::future<int> persisVar(const std::string& varName);

	// 缓存全部持久化：提交即返回，不阻塞，结果走 future
	std::future<int> persisVar();

	// 数据重写：将缓存和inDisk中的所有数据写入到另一个文件中，并删除旧文件
	std::future<int> reWrite();

	/*
		功能：读取缓存运行统计快照（命中/未命中/淘汰计数与耗时，供性能测试与运行观测）
		参数：无
		返回值：CacheStat（各字段含义见 cache.h 顶部结构体定义）
	*/
	CacheStat getStat() const;
};

#endif // !_CACHE_H_
