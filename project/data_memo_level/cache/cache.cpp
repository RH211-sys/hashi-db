#include "cache.h"
#include <vector>
#include <queue>
#include <random>


Cache::Cache(const long long memoSize, const int batchSize, const int upDisEdge, const int minDisEdge)
{
	this->memoSize = memoSize;
	this->curSize = 0;
	this->rwMutex = std::make_unique<WritePrefMutex>(batchSize, upDisEdge, minDisEdge);
}


/* =============== private =============== */

double Cache::evictScore(Val& val) {
	auto now = std::chrono::system_clock::now();
	// 距上次写入间隔（微秒），下限取 1：只用于查档位，不取对数，无 log(0) 问题
	// updateTime 只在写操作/回填等写路径更新，读命中不刷新（命中路径纯读零写）
	long long intervalUs = std::chrono::duration_cast<std::chrono::microseconds>(now - val.updateTime).count();
	if (intervalUs < 1) intervalUs = 1;

	// 时间档位（静态常量表）：<1min / <1h / <1d / <7d / ≥7d
	// 档距 30 > ln(dataSize) 上限（ln(memoSize) ≈ 20~25），跨档绝对无法靠大小翻盘
	static constexpr long long MIN_US = 60LL * 1000 * 1000;			// 1min
	static constexpr long long HOUR_US = 60LL * MIN_US;				// 1h
	static constexpr long long DAY_US = 24LL * HOUR_US;				// 1d
	static constexpr long long WEEK_US = 7LL * DAY_US;				// 7d
	static constexpr double LEVEL_DIS = 30.0;						// 档距
	int level = 0;
	if (intervalUs < MIN_US)		level = 0;
	else if (intervalUs < HOUR_US)	level = 1;
	else if (intervalUs < DAY_US)	level = 2;
	else if (intervalUs < WEEK_US)	level = 3;
	else							level = 4;

	// 分数 = 档位 * 档距 + ln(dataSize)；dataSize 下限取 1 防 ln(0)
	long long ds = val.dataSize > 0 ? val.dataSize : 1;
	return level * LEVEL_DIS + std::log(static_cast<double>(ds));
}

void Cache::evict()
{
	// 淘汰目标：淘汰到 curSize < 0.9 * memoSize（留 10% 余量，防刚淘汰完又超阈值抖动）
	// 由写线程调用（addData/modData/回填超限后触发），全程串行、阻塞执行至容量达标
	long long target = memoSize * 9 / 10;
	long long sizeNow = curSize;	// 当前大小（写线程串行上下文内维护）

	// 1. 先批量删过期数据（独占锁内全遍历：先清过期垃圾，避免把活数据误淘汰）
	//    全遍历只在写线程的淘汰路径发生（低频、阻塞任务），不在读路径上
	//    inDisk 索引一次批量删（磁盘集合删除接口），防过期数据从磁盘复活
	std::vector<std::string> expiredNames;
	{
		rwMutex->lock();
		auto now = std::chrono::system_clock::now();
		auto it = cache_db.begin();
		while (it != cache_db.end()) {
			Val& v = it->second;
			if (!v.isPermanent && now >= v.expireTime) {
				expiredNames.push_back(it->first);	// 更新删除集
				curSize -= v.dataSize;				// 更新缓存当前大小
				it = cache_db.erase(it);			// 删除该数据
			} else {
				++it;
			}
		}
		sizeNow = curSize;	// 锁内快照：解锁后循环条件用它，避免锁外读竞态
		rwMutex->unlock();
	}
	// 一次提交批量删（内存任务，微秒级）并等待完成，保证过期数据已从磁盘索引消失（防复活窗口）
	if (!expiredNames.empty()) {
		diskThread->submit([this, expiredNames = std::move(expiredNames)]() {
			return disk->delData(std::move(expiredNames));
		}, CACHE_TASK).get();	// 写线程短暂等待磁盘线程快速任务
	}
	if (sizeNow <= target) return;	// 删完过期已达目标，无需采样淘汰

	// 2. 全局采样淘汰（无 LRU 链表）：不遍历整个缓存，从 cache_db 随机取样本算分入大顶堆
	//    分数高 = 最优先淘汰；每轮直接取堆顶，堆空才补采一批（不每轮重采样）
	const int SAMPLE_NUM = 15;		// 样本量(预先设置，后续可能会统一封装)
	std::priority_queue<std::pair<double, std::string>> pq;	// <分数, 变量名>，pair 先比分数 = 大顶堆
	std::mt19937 rng(std::random_device{}());	// 采样随机源（淘汰低频，每次构造可接受）

	// 补采一批：随机起点桶 + 环形扫桶取样本（共享锁内只读 cache_db，不阻塞并发读）
	auto sampleBatch = [&]() {
		size_t bucketNum = cache_db.bucket_count();
		if (bucketNum == 0) return;	// 缓存空，无可采样
		size_t beginBucket = static_cast<size_t>(rng()) % bucketNum;	// 随机起点桶
		int got = 0;
		rwMutex->lock_shared();
		for (size_t step = 0; step < bucketNum && got < SAMPLE_NUM; ++step) {
			size_t bucket = (beginBucket + step) % bucketNum;	// 环形扫桶
			for (auto it = cache_db.begin(bucket); it != cache_db.end(bucket) && got < SAMPLE_NUM; ++it) {
				pq.emplace(evictScore(it->second), it->first);
				++got;
			}
		}
		rwMutex->unlock_shared();
	};

	while (sizeNow > target) {
		if (pq.empty()) {
			sampleBatch();	// 候选耗尽，补采一批
			if (pq.empty()) break;	// 缓存空，无数据可淘汰
		}
		std::string victim = pq.top().second;
		pq.pop();

		// 采样与淘汰执行之间数据可能已被改：每轮删除前在独占锁内重新校验（以当时为准），无需跨轮大锁
		bool dirty = false;	// 脏数据还需落盘（磁盘以缓存数据为准）；干净数据与磁盘一致，可直删
		Val val;			// 脏数据：锁内拷贝出 Val（shared_ptr 浅拷贝，实体零拷贝），随刷盘任务带走
		{
			rwMutex->lock();
			auto it = cache_db.find(victim);
			if (it == cache_db.end()) {
				rwMutex->unlock();
				continue;	// 已被并发删除，丢弃该候选
			}
			dirty = it->second.isDirty;
			if (!dirty) {
				// 干净数据：锁内直接删（磁盘已有副本，inDisk 不动，可读回）
				curSize -= it->second.dataSize;	// 更新缓存当前大小
				cache_db.erase(it);				// 删除该数据
			}
			else {
				// 脏数据：Val 拷贝出（shared_ptr 只增引用计数），随后删缓存减 curSize，不等待刷盘结果
				val = it->second;
				curSize -= val.dataSize;	// 拷贝后从 val 取大小
				cache_db.erase(it);
			}
			sizeNow = curSize;	// 锁内快照：解锁后循环条件用它，避免锁外读竞态
			rwMutex->unlock();
		}
		/* 
			若干净数据已删除，则处理下一个候选 
			否则是脏数据：提交刷盘任务，提交即返回，不等待
		*/
		if (dirty) {	
			diskThread->push([this, victim, val = std::move(val)]() {
				disk->persisData(victim, val);
				}, DISK_TASK);
		}
	}
}

/* =============== public =============== */

void Cache::bind(std::shared_ptr<DiskThread> diskThread, std::shared_ptr<Disk> d, std::shared_ptr<ReadPool> readPool, std::shared_ptr<WriteThread> writeThread)
{
	this->disk = d;
	this->diskThread = diskThread;
	this->readPool = readPool;
	this->writeThread = writeThread;
}

std::future<SelResult> Cache::selData(const std::string& varName)
{
	// 查询链：读线程池执行（命中纯读零写，不产生任何写任务），结果经 future 带回
	return readPool->submit([this, varName]() -> SelResult {
		/* 1. 共享锁探测缓存 + 过期判定 */
		bool inCache = false, expired = false;
		std::shared_ptr<std::any> entity;	// 命中：锁内拷贝实体指针（只拷贝指针，实体零拷贝）
		{
			rwMutex->lock_shared();
			auto it = cache_db.find(varName);
			if (it != cache_db.end()) {
				inCache = true;
				expired = !it->second.isPermanent && it->second.expireTime <= std::chrono::system_clock::now();
				if (!expired) entity = it->second.entity;	// 与缓存共享同一实体指针
			}
			rwMutex->unlock_shared();
		}

		if (inCache && !expired) {
			// 命中未过期：直接返回实体指针（不刷新时间/顺序信息，命中路径零写零任务）
			return SelResult{ SUCCESS, std::move(entity) };
		}
		if (inCache && expired) {
			// 命中已过期：返回 EXPIRED；清理交两条平级任务（写线程删缓存 + 磁盘线程删 inDisk），提交即返回不等待
			writeThread->push([this, varName]() {
				rwMutex->lock();	// 删缓存是写操作，独占锁
				auto it = cache_db.find(varName);
				if (it != cache_db.end()) {	// 排队期间可能已被并发删除，跳过
					curSize -= it->second.dataSize;	// 更新缓存当前大小
					cache_db.erase(it);				// 删除该数据
				}
				rwMutex->unlock();
			});
			diskThread->push([this, varName]() { disk->delData(varName); }, CACHE_TASK);	// 删磁盘索引，防复活
			return SelResult{ EXPIRED, nullptr };
		}

		/* 2. 未命中：读盘在磁盘线程任务内完成"读盘 + 过期判定 + 过期直删"，读池任务等待这一轮磁盘 IO */
		auto fut = diskThread->submit([this, varName]() -> std::pair<int, Val> {
			std::any res;	// 读出的实体（本任务局部变量，不引用调用方栈帧）
			Val val;		// 读出的时间信息（是否永久/过期时间/更新时间等）
			int code = disk->selData(varName, res, val);
			if (code != SUCCESS) return std::make_pair(code, Val{});	// 磁盘也没有/读失败
			// 已过期：已在磁盘线程，直接删除（不二次提交删除任务），数据舍弃不回填
			if (!val.isPermanent && val.expireTime <= std::chrono::system_clock::now()) {
				disk->delData(varName);	// 过期数据：删磁盘索引防复活
				return std::make_pair(EXPIRED, Val{});
			}
			val.entity = std::make_shared<std::any>(std::move(res));	// 读出的实体包进指针，随结果带回
			return std::make_pair(SUCCESS, std::move(val));
		}, DISK_TASK);	// 读文件，磁盘 IO 任务

		auto ret = fut.get();	// 读池任务等待磁盘线程读盘
		int code = ret.first;
		if (code != SUCCESS) return SelResult{ code, nullptr };

		// 3. 磁盘读回且未过期：回填交写线程后台（push 不等待；闭包值捕获含 shared_ptr 的 Val，读出实体保活）
		//    返回的实体指针先拷出一份（与回填共享同一实体），上层拿到后写线程再回填也不冲突
		Val val = std::move(ret.second);
		std::shared_ptr<std::any> retEntity = val.entity;	// 返回给上层的实体指针
		writeThread->push([this, varName, val = std::move(val)]() mutable {	// mutable：任务内要修改捕获的 Val（标脏/更新时间）
			rwMutex->lock();	// 回填是写操作，独占锁
			if (!cache_db.contains(varName)) {	// 锁内重校验：排队期间可能已并发插入，存在则跳过
				val.isDirty = false;	// 磁盘读回的数据是干净的
				val.updateTime = std::chrono::system_clock::now();	// 回填视为一次写入访问（写路径才更新）
				long long dataSize = val.dataSize;	// move 前先取大小
				cache_db.emplace(varName, std::move(val));	// 移动进缓存，实体指针零拷贝
				curSize += dataSize;			// 更新缓存当前大小
			}
			long long sizeNow = curSize;	// 锁内快照：解锁后判断是否触发淘汰
			rwMutex->unlock();
			// 回填会增大缓存：超限触发淘汰（全局采样，写线程内阻塞执行）
			if (sizeNow > memoSize) evict();
		});
		return SelResult{ SUCCESS, std::move(retEntity) };	// 与回填同一实体指针，返回"成功"
	});
}

std::future<int> Cache::delData(const std::string& varName)
{
	// 删除是写操作：缓存删 + 磁盘索引删整条链封装为一个写线程任务
	// 调用线程不阻塞（提交任务后立即返回 future）；淘汰阻塞执行时本任务在写线程队列内自然等待
	return writeThread->submit([this, varName]() -> int {
		// 1. 独占锁内删缓存条目（命中即删除已生效）
		bool cacheHit = false;	// 缓存是否命中（命中即已删除，磁盘无记录不算失败）
		{
			rwMutex->lock();	// 写锁统一由写线程执行
			auto it = cache_db.find(varName);
			if (it != cache_db.end()) {
				curSize -= it->second.dataSize;	// 更新缓存当前大小
				cache_db.erase(it);				// 命中：从缓存移除
				cacheHit = true;
			}
			rwMutex->unlock();
		}

		// 2. 磁盘索引删除：短暂等待磁盘线程快速任务（CACHE_TASK 微秒级：查 inDisk 删索引，只动内存不进文件）
		//    缓存命中但磁盘无记录（数据从未落盘）：删除已生效，返回成功；缓存磁盘都没有才算未找到
		std::future<int> fut = diskThread->submit([this, varName, cacheHit]() {
			int code = disk->delData(varName);
			return (code == FIND_FAILED && cacheHit) ? SUCCESS : code;
		}, CACHE_TASK);
		return fut.get();	// 写线程短暂等待磁盘线程快速任务
	});
}

std::future<int> Cache::persisVar(const std::string& varName)
{
	/*
		bool varExist = true;
		{
			rwMutex->lock_shared();
			if (!cache_db.contains(varName)) {
				varExist = false;
			}
			rwMutex->unlock_shared();
		}
	// 未找到该变量，无法进行持久化
	if (!varExist) return FIND_FAILED;
	*/

	// 由于考虑到磁盘异步处理的时候，cache_db可能因为并发把数据删了，所以直接给磁盘线程检查，缓存模块不检查，防止磁盘访问不到该数据后崩溃

	// 找到该数据，进行持久化（写文件，磁盘 IO 任务），提交即返回，结果走 future
	return diskThread->submit([this, varName]() { return disk->persisData(varName); }, DISK_TASK);
}

std::future<int> Cache::persisVar()
{
	// 持久化缓存中所有脏数据：磁盘线程执行 persisAll（内部遍历 cache_db 收集脏数据并写文件）
	// 提交即返回，结果走 future
	return diskThread->submit([this]() { return disk->persisAll(); }, DISK_TASK);
}

std::future<int> Cache::reWrite()
{
	// 直接交给disk一起处理，降低业务逻辑耦合，更易维护（读写文件，磁盘 IO 任务）
	// 提交即返回，结果走 future（需要确认重写完成时 get()）
	return diskThread->submit([this]() { return disk->reWrite(); }, DISK_TASK);
}
