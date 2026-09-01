#include "cache.h"
#include <vector>
#include <queue>


Cache::Cache(const long long memoSize, const int batchSize, const int upDisEdge, const int minDisEdge)
{
	this->memoSize = memoSize;
	this->curSize = 0;
	this->rwMutex = std::make_unique<WritePrefMutex>(batchSize, upDisEdge, minDisEdge);

	// LRU 链表初始化：哨兵头尾互链，实际节点挂在中间
	start = new ListNode();
	end = new ListNode();
	start->next = end;
	end->pre = start;
}


/* =============== private =============== */

double Cache::LRU_score(Val& val) {
	auto now = std::chrono::system_clock::now();
	// 最近使用间隔（微秒），下限取 1：只用于查档位，不取对数，无 log(0) 问题
	long long intervalUs = std::chrono::duration_cast<std::chrono::microseconds>(now - val.updateTime).count();
	if (intervalUs < 1) intervalUs = 1;

	// 时间档位（静态常量表）：<1min / <1h / <1d / <7d / ≥7d
	// 档距 30 > ln(dataSize) 上限（ln(memoSize) ≈ 20~25），跨档绝对无法靠大小翻盘
	static constexpr long long MIN_US = 60LL * 1000 * 1000;			// 1min
	static constexpr long long HOUR_US = 60LL * MIN_US;				// 1h
	static constexpr long long DAY_US = 24LL * HOUR_US;				// 1d
	static constexpr long long WEEK_US = 7LL * DAY_US;				// 7d
	int level = 0;
	if (intervalUs < MIN_US)		level = 0;
	else if (intervalUs < HOUR_US)	level = 1;
	else if (intervalUs < DAY_US)	level = 2;
	else if (intervalUs < WEEK_US)	level = 3;
	else							level = 4;

	// 分数 = 档位 * 档距 + ln(dataSize)；dataSize 下限取 1 防 ln(0)
	long long ds = val.dataSize > 0 ? val.dataSize : 1;
	return level * 30.0 + std::log(static_cast<double>(ds));
}

void Cache::LRU_out()
{
	// 淘汰目标：curSize < 0.9 * memoSize（留 10% 余量，防刚淘汰完又超阈值抖动）
	long long target = memoSize * 9 / 10;
	if (curSize <= target) return;

	// 1. 先删过期数据：缓存删 + 链表删 + curSize 减（写锁内）
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
				LRU_removeNode(it->first);			// 更新链表
				curSize -= v.dataSize;				// 更新缓存当前大小
				it = cache_db.erase(it);			// 删除该数据
			} else {
				++it;
			}
		}
		rwMutex->unlock();
	}
	// 一次提交批量删（内存任务，微秒级），不逐个提交任务
	if (!expiredNames.empty()) {
		diskThread->submit([this, expiredNames]() { return disk->delData(move(expiredNames)); }, CACHE_TASK).get();
	}

	// 2. 尾部采样淘汰：优先队列（大顶堆）每批采样尾部 15 个算好分数，
	//    后续轮次直接从堆顶取，避免每轮重复遍历同一批尾部元素（O(15n) → O(n·log15)）
	const int SAMPLE_NUM = 15;		// 样本量(预先设置，后续可能会统一封装)
	std::priority_queue<std::pair<double, std::string>> pq;	// <分数, 变量名>，pair 先比分数 = 大顶堆

	// 补采一批：写锁内从链表尾部（最久未用区域）取 SAMPLE_NUM 个节点算分数入堆
	auto sampleBatch = [&]() {
		int cnt = 0;
		rwMutex->lock();
		for (ListNode* node = end->pre; node != start && cnt < SAMPLE_NUM; node = node->pre, ++cnt) {
			auto it = cache_db.find(node->key);
			if (it == cache_db.end()) continue;	// 链表与缓存不一致（不应发生），跳过
			pq.emplace(LRU_score(it->second), node->key);
		}
		rwMutex->unlock();
	};

	while (curSize > target) {
		if (pq.empty()) {
			sampleBatch();	// 候选耗尽，补采一批
			if (pq.empty()) break;	// 链表空，无数据可淘汰
		}
		std::string victim = pq.top().second;
		pq.pop();

		bool dirty = false;		// 如果当前数据是脏数据，则还需要更新磁盘数据，如果不是脏数据，说明和磁盘一致，可以删除
		Val val;				// 脏数据：锁内从迭代器移动出来，随刷盘任务带走（磁盘线程不依赖缓存）
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
				LRU_removeNode(victim);
				curSize -= it->second.dataSize;
				cache_db.erase(it);
			} else {
				// 脏数据：Val 移动进任务闭包（零拷贝），随后立即删缓存减 curSize，不等待刷盘结果
				val = std::move(it->second);
				LRU_removeNode(victim);
				curSize -= val.dataSize;	// 移动后从 val 取大小
				cache_db.erase(it);
			}
			rwMutex->unlock();
		}
		if (!dirty) continue;	// 干净数据已删除，处理下一个候选

		// 脏数据：提交刷盘任务（数据已随闭包走，磁盘线程不再查缓存），提交即返回，不等待结果
		diskThread->submit([this, victim, val = std::move(val)]() {
			return disk->persisData(victim, val);
		}, DISK_TASK);
	}
}

void Cache::LRU_addNode(const std::string& varName)
{
	ListNode* node = new ListNode(varName);
	node->next = start->next;
	node->pre = start;
	start->next->pre = node;
	start->next = node;
	LRU_hash.emplace(varName, node);
}

void Cache::LRU_moveToHead(const std::string& varName)
{
	auto it = LRU_hash.find(varName);
	if (it == LRU_hash.end()) return;	// 理论上不会走到

	ListNode* node = it->second;
	// 摘除
	node->pre->next = node->next;
	node->next->pre = node->pre;
	// 头插
	node->next = start->next;
	node->pre = start;
	start->next->pre = node;
	start->next = node;
}

void Cache::LRU_removeNode(const std::string& varName)
{
	auto it = LRU_hash.find(varName);
	if (it == LRU_hash.end()) return;

	ListNode* node = it->second;
	node->pre->next = node->next;
	node->next->pre = node->pre;
	delete node;
	LRU_hash.erase(it);
}

/* =============== public =============== */

void Cache::setDisk(std::shared_ptr<Disk> d)
{
	this->disk = d;
}

void Cache::setDiskThread(std::shared_ptr<DiskThread> t)
{
	this->diskThread = t;
}

void Cache::selData(const std::string& varName, std::any& res, int& resCode) {
	// 获取读共享锁
	rwMutex->lock_shared();
	// 命中缓存直接返回（缓存数据必定最新，且效率高）
	if (cache_db.contains(varName)) {
		res = cache_db.at(varName).entity;	// 返回值的实体，不是整个 Val
		rwMutex->unlock_shared();
		// 命中刷新：短写锁更新访问时间 + 移到链表头（读锁内不能升级写锁，解锁后重新查）
		rwMutex->lock();
		auto it = cache_db.find(varName);
		if (it != cache_db.end()) {	// 解锁后可能已被并发删除
			it->second.updateTime = std::chrono::system_clock::now();	// 刷新访问时间
			LRU_moveToHead(varName);	// 移到链表头（最近使用）
		}
		rwMutex->unlock();
		resCode = SUCCESS;
		return;
	}
	// 未命中：先解锁，再走磁盘查询，避免持锁做磁盘 IO
	rwMutex->unlock_shared();

	// 提交磁盘读任务，只有发起者 get() 阻塞等待结果，其他线程不受影响
	// disk 侧找到变量时填好 res（实体）和 val（时间信息：更新时间/是否永久/过期时间）
	Val val;
	std::future<int> fut = diskThread->submit([this, &varName, &res, &val]() {
		return disk->selData(varName, res, val);
	}, DISK_TASK);	// 读文件，磁盘 IO 任务
	int code = fut.get();
	if (code != SUCCESS) {
		resCode = code;	// 磁盘也找不到
		return;
	}

	// 回填前检查过期：过期数据舍弃（不回填缓存），磁盘记录提交删除任务清理（防复活）
	// val 是本次查询的局部数据，无并发，锁外检查
	{
		auto now = std::chrono::system_clock::now();
		if (!val.isPermanent && val.expireTime <= now) {
			diskThread->submit([this, varName]() { return disk->delData(varName); }, CACHE_TASK);
			resCode = EXPIRED;	// 数据已过期
			return;
		}
	}

	// 回填缓存前重新校验 key（可能已被并发修改/删除），校验通过才回填
	// disk 已填时间信息，缓存只补 isDirty 和 entity
	{
		rwMutex->lock();	// 回填是写操作，拿写锁
		if (!cache_db.contains(varName)) {
			val.isDirty = false;	// 磁盘数据是干净的
			val.entity = res;		// 实体从磁盘读回
			val.updateTime = std::chrono::system_clock::now();	// 回填视为一次访问
			cache_db.emplace(varName, std::move(val));	// 移动语义，避免拷贝
			LRU_addNode(varName);			// 新数据头插（最近使用）
			curSize += val.dataSize;		// 更新缓存当前大小
		}
		rwMutex->unlock();
	}
	resCode = SUCCESS;
	// 回填会增大缓存：超限触发淘汰
	if (curSize > memoSize) LRU_out();
}

std::future<int> Cache::delData(const std::string& varName)
{
	// 锁内只做"查 + 删"，粒度最小
	rwMutex->lock();
	auto it = cache_db.find(varName);
	if (it != cache_db.end()) {
		LRU_removeNode(varName);		// 更新链表
		curSize -= it->second.dataSize;	// 更新缓存当前大小
		cache_db.erase(it);				// 命中：从缓存移除
	}
	rwMutex->unlock();

	// 提交磁盘删除任务：磁盘线程检查 inDisk（存在则标记 delDisk），只动内存标记不进文件
	// 任务不引用捕获调用者变量（异步执行时调用者栈帧可能已销毁），结果走 future 通道
	return diskThread->submit([this, varName]() {
		return disk->delData(varName);
	}, CACHE_TASK);
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
