#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <random>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <cstdint>
#include <cmath>
#include <future>

#include "data_memo_level/controller/controller.h"
#include "data_memo_level/data_type.h"

/*
	性能测试（memo_test3）：存储层写并发版基准
	============================================================
	本文件以 test2.cpp 为蓝本（负载定义、数据分布、场景划分、输出格式保持一致），
	核心增强：**写路径并发化压测**（这是相对 test2 的唯一关键差异）。

	test2 的压测循环是"submit → 立即 get()"：
	  每个压测线程同时在途请求 = 1 → 总在途 = WORKERS(8)。
	  读7写3 时在途写 ≈ 8 × 0.3 = 2.4，全写时 = 8 → 写线程池(4线程)根本打不满，
	  因此测不出"记录锁 + 写池"的并发收益。

	test3 的压测循环对**写请求**使用"环形在途窗口"（ring of futures）：
	  每线程始终维持 WRITE_INFLIGHT 个在途 modData（完成一个才补一个）
	  → 总在途写 = WORKERS × WRITE_INFLIGHT（默认 8 × 8 = 64），远超写池线程数，
	  真正压出多线程写能力；读请求仍同步 submit+get（读并发由读池承载，与 test2 一致）。

	场景与 test2 相同：
	  1. QPS 档：全读 / 读7写3 / 全写（各 30s），记录 QPS + mean/P50/P99/P999
	  2. 命中率档：总数据 = 缓存 2x/3x/4x，纯读 + 冷热 80/20，记录 hit/miss/hit-rate
	  3. evict 统计：evict calls/items/总耗时 → evict-ratio（淘汰是否拖慢写）
	  4. 数据特征：单条 1K~10M 档位分布 / 对数正态（与 test2 相同），内容随机填充
	  5. 写场景注入线程：每 30ms add 一条新 key，持续制造淘汰压力；
	     预热结束 flushDisk().get() 做刷盘屏障（脏数据全部落盘后再测）

	输出格式与 test2 完全一致。设 WRITE_INFLIGHT=1 即退化为 test2 的同步写行为，可作对照。
*/

// ============ 配置 ============
// 测试比例基准（缓存 : 数据集 : 磁盘 ≈ 1 : 3 : 5），按业界容量建议推导（非行业标准比例，见下）：
//   - Redis Enterprise Auto Tiering（内存热层 + 闪存冷层，与本项目模型一致）：
//     RAM 保留全部 key/索引 + 热数据(工作集)，建议 RAM 至少占总 value 的 20%，
//     闪存容量 ≥ 总数据，并另留写放大/缓冲余量；适用"工作集<<数据集 + 热点明显"，
//     不适用"访问均匀 / 工作集≈数据集"（那会把淘汰抖动当正常业务来测）。
//   - 80/20 热冷：缓存按"热集"而非全量定容（cache ≈ 热集字节 + 余量）。
//   - 换算：缓存100M ≈ 数据集300M 的 33%（贴合"RAM≥20%值 + 热集余量"），磁盘500M ≈ 1.67×数据
//     （含压缩/写放大余量），凑整为 1:3:5；对比参考：Pika/SSD 型是"数据全落盘+内存小缓冲"，
//     不是本项目的热冷分层模型。
//   详见 doc/arch/data_memo_doc/实现阶段/二轮优化/（优化1.md / 测试比例讨论）
constexpr long long MEMO_SIZE = 1000LL * 1024 * 1024;	// 缓存 100M（1）
constexpr long long DISK_SIZE = 500LL * 1024 * 1024;	// 磁盘 500M（5，≥ 数据集 + 压缩/写放大余量）
constexpr int POOL_THREADS = 8;							// 读/写线程池线程数
constexpr int WRITE_INFLIGHT = 4;						// 每线程在途写请求数（窗口深度；=1 退化为 test2 同步写）
const std::string DB_NAME = "test3_data.dat";			// 数据文件
// 数据集档位（3）：
//   QPS 正常档 300M（≈1:3:5）；命中率对照档 2x/3x(=200M/300M，对应 1:2:5 / 1:3:5)。
//   唯一数据总量始终 ≤ 磁盘 500M，磁盘上限不被"不断新增的唯一数据"顶穿；
//   热更新产生的版本 churn 由磁盘自动压缩回收，不计入唯一数据。
constexpr long long WARM_BYTES = 30LL * 1024 * 1024;	// QPS 档数据集（预热池）300M
constexpr int INJECT_KEYS = 60;							// 缓慢新增新 key：60 条（≈24MB，数据集 300→~324M，≈1:3.2:5）
constexpr double RUN_SECONDS = 30.0;					// 每场景固定时长
constexpr int WORKERS = 8;								// 压测线程数
constexpr int INJECT_MS = 500;							// 注入间隔（≈2 key/s）：模拟数据集缓慢增长，不做注入洪流
constexpr size_t SAMPLE_CAP = 200000;					// 延迟样本蓄水池上限 / 线程

// ============ 测试数据实体（1K~10M 随机内容） ============

struct BenchData {
	long long size;				// 数据长度（随机内容）
	std::vector<char> data;		// 随机填充的数据体

	static std::string getClassName() { return "BenchData"; }
	template <class Archive>
	void serialize(Archive& ar) { ar(size, data); }
	static long long theSize(const BenchData& b) { return sizeof(BenchData) + b.data.size(); }
};

// ============ 随机工具 ============

static double unit(std::mt19937& rng) { return static_cast<double>(rng()) / static_cast<double>(rng.max()); }

// 单条大小档位分布，覆盖 1K~10M 全跨度（与 test2 一致）：
//   70% [1K, 16K]   / 25% [16K, 1M]   / 5% [1M, 10M]
static long long randSizeTiered(std::mt19937& rng) {
	constexpr long long K = 1024;
	double u = unit(rng);
	if (u < 0.70) {			// 小档：1K ~ 16K
		return 1 * K + static_cast<long long>(static_cast<double>(15 * K) * (u / 0.70));
	} else if (u < 0.95) {	// 中档：16K ~ 1M
		double t = (u - 0.70) / 0.25;
		return 16 * K + static_cast<long long>(static_cast<double>(1024 * K - 16 * K) * t);
	} else {				// 大档：1M ~ 10M
		double t = (u - 0.95) / 0.05;
		return 1024 * K + static_cast<long long>(static_cast<double>(9LL * 1024 * K) * t);
	}
}

// 数据大小对数正态（log10 均值 5.0、σ 0.5，覆盖 1K~10M），与 test2 一致（命中率档用）
static long long randSizeLogNormal(std::mt19937& rng) {
	thread_local std::normal_distribution<double> dist(5.0, 0.5);	// 分布有内部状态，每线程独立
	double lg = dist(rng);
	if (lg < 3.0) lg = 3.0;	// 钳位 1K
	if (lg > 7.0) lg = 7.0;	// 钳位 10M
	return static_cast<long long>(std::pow(10.0, lg));
}

// 写尺寸档（"更新同量级数据"）：4K ~ 1M 对数均匀（均值 ~190KB）
// 相比 randSizeTiered 的 1M~10M 顶档，避免"每次随机写都可能蹦出 10M 版本"造成的版本放大；
// 保留 1M 上限用于触发容量/淘汰，但把放大压低到贴近真实业务更新。
static long long randSizeUpdate(std::mt19937& rng) {
	static thread_local std::uniform_real_distribution<double> dist(12.0, 20.0);	// 2^12=4K .. 2^20=1M
	return static_cast<long long>(std::pow(2.0, dist(rng)));
}

// 构造随机内容的实体（内容不跨进程使用，字节序无约束）
static BenchData makeData(long long size, std::mt19937& rng) {
	BenchData b;
	b.size = size;
	b.data.resize(size);
	std::mt19937_64 r64{ rng() };
	std::uint64_t tmp;
	size_t i = 0;
	for (; i + 8 <= b.data.size(); i += 8) {
		tmp = r64();
		std::memcpy(b.data.data() + i, &tmp, 8);
	}
	for (; i < b.data.size(); ++i) {
		b.data[i] = static_cast<char>(rng() & 0xFF);
	}
	return b;
}

// ============ key 池 ============

struct PoolEntry {
	std::string key;	// key 名
	long long size;		// 该 key 的数据大小（预热用）
};

// 按字节预算建 key 池（与 test2 一致）：逐条按大小生成器取样累加，超过预算停止
static std::pair<std::vector<PoolEntry>, long long> buildPool(
	long long budgetBytes, std::mt19937& rng,
	long long (*sizeGen)(std::mt19937&) = &randSizeTiered) {
	std::vector<PoolEntry> pool;
	long long total = 0;
	for (int i = 0; total < budgetBytes; ++i) {
		long long sz = sizeGen(rng);
		pool.push_back({ "key_" + std::to_string(i), sz });
		total += sz;
	}
	return { std::move(pool), total };
}

// 预热：串行 add 全部 key（get() 确认成功），结束后 flushDisk().get() 做刷盘屏障
static void warmUp(Controller& db, std::vector<PoolEntry>& pool, std::mt19937& rng) {
	int fail = 0;
	for (auto& e : pool) {
		int code = db.addData(e.key, makeData(e.size, rng), true, std::chrono::seconds(0)).get();
		if (code != SUCCESS) ++fail;
	}
	if (fail > 0) std::cout << "  [warn] warmUp: " << fail << " adds failed" << std::endl;
	db.flushDisk().get();
}

// ============ 延迟采样与压测循环 ============

// 延迟样本蓄水池（单位 ns），与 test2 一致
struct Reservoir {
	std::vector<long long> buf;
	size_t seen = 0;

	void add(long long ns, std::mt19937& rng) {
		if (seen < SAMPLE_CAP) {
			buf.push_back(ns);
			++seen;
			return;
		}
		++seen;
		size_t j = static_cast<size_t>(rng()) % seen;	// 蓄水池替换判定
		if (j < SAMPLE_CAP) buf[j] = ns;
	}
};

struct LoadResult {
	long long total = 0;	// 完成的请求数
	double qps = 0;			// 请求数 / 时长
	double meanUs = 0;		// 平均延迟
	double p50Us = 0;		// P50 延迟
	double p99Us = 0;		// P99 延迟
	double p999Us = 0;		// P999 延迟
};

/*
	压力循环（test3 写并发版）：
	- 读请求：同步 submit + get()（与 test2 相同；读并发 = WORKERS）
	- 写请求：环形在途窗口（与 test2 的关键差异）
	  —— 每线程预填 WRITE_INFLIGHT 个在途 modData，每次"取回最旧完成的"再补一个新写，
	     稳态在途写 = WORKERS × WRITE_INFLIGHT，打满写线程池（POOL_THREADS=4）
	- readRate：读概率，其余为写（写 = modData 热更新）
	- hotSpot：请求分布 20% key（池前 1/5）承担 80% 请求
	- injectPool 非空：注入线程每 INJECT_MS 顺次 add 一条新 key（持续制造淘汰压力）
*/
static LoadResult runLoad(Controller& db, const std::vector<PoolEntry>& pool,
	double readRate, bool hotSpot, double seconds, const std::vector<PoolEntry>* injectPool = nullptr) {
	std::atomic<bool> stop{ false };
	std::vector<long long> done(WORKERS, 0);
	std::vector<Reservoir> samples(WORKERS);
	const size_t hotNum = pool.size() / 5;	// 热区 = 池前 20% key

	// 请求 key 选取：80% 概率热区（前 20% key），20% 概率冷区
	auto pickIdx = [&](std::mt19937& rng) -> size_t {
		if (hotSpot && hotNum > 0 && unit(rng) < 0.80) {
			return rng() % hotNum;
		}
		return hotNum + rng() % (pool.size() - hotNum);
	};

	std::vector<std::thread> workers;
	for (int w = 0; w < WORKERS; ++w) {
		workers.emplace_back([&, w]() {
			std::mt19937 rng(0x5EED + w * 7919);
			long long localDone = 0;

			// 写环形在途窗口：每个槽 = 一个已提交未取回的 modData + 其提交时刻
			// （key 随机选取，避免全部打同一 key 被记录锁串行）
			struct WriteSlot {
				std::future<int> fut;	// 在途写请求
				long long submitNs;		// 提交时刻（ns，算端到端延迟用）
			};
			std::vector<WriteSlot> win(WRITE_INFLIGHT);
			// 只有 readRate < 1.0 时才预填写窗口
			if (readRate < 1.0) {
				for (int i = 0; i < WRITE_INFLIGHT; ++i) {
					size_t idx = pickIdx(rng);
					auto t0 = std::chrono::steady_clock::now();
					win[i].submitNs = std::chrono::duration_cast<std::chrono::nanoseconds>(t0.time_since_epoch()).count();
					win[i].fut = db.modData(pool[idx].key, std::move(makeData(randSizeUpdate(rng), rng)));
				}
			}
			int winHead = 0;	// 当前要取回的最旧写槽

			while (!stop.load(std::memory_order_relaxed)) {
				bool isRead = unit(rng) < readRate;
				size_t idx = pickIdx(rng);
				if (isRead) {
					// 读：同步提交 + get()（与 test2 相同，读并发由读池承载）
					auto t0 = std::chrono::steady_clock::now();
					auto res = db.selData(pool[idx].key).get();
					(void)res;
					long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - t0).count();
					samples[w].add(ns, rng);
				} else {
					// 写：取回最旧完成的写 → 记录延迟 → 原地补一个新写（窗口保持满）
					WriteSlot& s = win[winHead];
					int code = s.fut.get();
					(void)code;
					long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now().time_since_epoch()).count() - s.submitNs;
					samples[w].add(ns, rng);

					auto t0 = std::chrono::steady_clock::now();
					s.submitNs = std::chrono::duration_cast<std::chrono::nanoseconds>(t0.time_since_epoch()).count();
					s.fut = db.modData(pool[idx].key, std::move(makeData(randSizeUpdate(rng), rng)));
					winHead = (winHead + 1) % WRITE_INFLIGHT;
				}
				++localDone;
			}
			done[w] = localDone;
		});
	}

	// 注入线程：限速 add 新 key（新增数据流），池耗尽或到时自动停止
	std::thread injector;
	if (injectPool != nullptr) {
		injector = std::thread([&]() {
			std::mt19937 rng(0xACE1);
			for (auto& e : *injectPool) {
				std::this_thread::sleep_for(std::chrono::milliseconds(INJECT_MS));
				if (stop.load(std::memory_order_relaxed)) break;
				db.addData(e.key, makeData(e.size, rng), true, std::chrono::seconds(0)).get();
			}
		});
	}

	// 主控：跑满固定时长后停表
	std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(seconds * 1000)));
	stop.store(true, std::memory_order_relaxed);
	for (auto& t : workers) t.join();
	if (injector.joinable()) injector.join();

	// 汇总：合并蓄水池样本排序取分位
	LoadResult out;
	long long totalNs = 0;
	std::vector<long long> all;
	for (int w = 0; w < WORKERS; ++w) {
		out.total += done[w];
		for (long long ns : samples[w].buf) {
			totalNs += ns;
			all.push_back(ns);
		}
	}
	std::sort(all.begin(), all.end());
	if (!all.empty()) {
		out.qps = out.total / seconds;
		out.meanUs = static_cast<double>(totalNs) / all.size() / 1000.0;
		out.p50Us = static_cast<double>(all[all.size() / 2]) / 1000.0;
		out.p99Us = static_cast<double>(all[static_cast<size_t>(all.size() * 0.99)]) / 1000.0;
		out.p999Us = static_cast<double>(all[static_cast<size_t>(all.size() * 0.999)]) / 1000.0;
	}
	return out;
}

// ============ 输出 ============

// 输出：详细数据报告（QPS + 延迟分位 + 命中率 + 淘汰统计；磁盘探针明细见 printDiskIo）
static void printLoad(const char* title, const LoadResult& r, const CacheStat& s, double seconds) {
	long long seen = s.hit + s.miss;
	double hitRate = seen > 0 ? 100.0 * s.hit / seen : 0.0;
	double runMs = seconds * 1000.0;
	double evictAvgMs = s.evictCnt > 0 ? static_cast<double>(s.evictUs) / s.evictCnt / 1000.0 : 0.0;
	double evictRatio = runMs > 0 ? 100.0 * (s.evictUs / 1000.0) / runMs : 0.0;
	std::cout << "===== " << title << " (duration " << seconds << "s) =====" << std::endl;
	std::cout << "  requests: " << r.total << " | QPS " << static_cast<long long>(r.qps) << std::endl;
	std::cout << "  latency(us): mean " << static_cast<long long>(r.meanUs)
		<< " | P50 " << static_cast<long long>(r.p50Us)
		<< " | P99 " << static_cast<long long>(r.p99Us)
		<< " | P999 " << static_cast<long long>(r.p999Us) << std::endl;
	std::cout << "  cache: hit " << s.hit << " / miss " << s.miss
		<< " -> hit rate " << hitRate << "%" << std::endl;
	std::cout << "  evict: " << s.evictCnt << " calls / " << s.evictItems << " items"
		<< " / avg " << evictAvgMs << " ms per call / evict-ratio " << evictRatio << "%" << std::endl;
}

// ============ 磁盘 IO 阶段占比输出（探针数据报告：#if 1 开启 / #if 0 关闭）============
#if 0
// 序列化 / 文件写 / flush / 磁盘读 / 队列积压与 worker 忙碌（占比按 RUN_SECONDS 计算）
static void printDiskIo(const DiskIoStat& io, const DiskQueueStat& q, double seconds) {
	double runMs = seconds * 1000.0;
	std::cout << "  disk-write: " << io.writeCnt << " recs / " << (io.writeBytes / 1024 / 1024) << " MB" << std::endl;
	std::cout << "    build(serialize) " << (io.buildUs / 1000) << " ms | file-write " << (io.fileUs / 1000)
		<< " ms | flush " << (io.flushUs / 1000) << " ms (" << io.flushCnt << " calls)" << std::endl;
	std::cout << "    per-write avg: build " << (io.writeCnt ? io.buildUs / io.writeCnt : 0)
		<< " us | file " << (io.writeCnt ? io.fileUs / io.writeCnt : 0) << " us" << std::endl;
	std::cout << "  disk-ovw/holes: overwrite " << io.overwriteCnt
		<< " (same " << io.overwriteSame << " / shrink " << io.overwriteShrink << ")"
		<< " | hole-use " << io.holeUseCnt
		<< " | holes " << io.holeCnt << " segs / " << (io.holeBytes / 1024 / 1024) << " MB" << std::endl;
	std::cout << "  disk-read: " << io.readCnt << " reads / " << (io.readUs / 1000) << " ms"
		<< " (avg " << (io.readCnt ? io.readUs / io.readCnt : 0) << " us)" << std::endl;
	double inDiskMissRatio = io.selCalls > 0 ? 100.0 * io.selInDiskMiss / io.selCalls : 0.0;
	std::cout << "  disk-sel: calls " << io.selCalls << " | inDisk-miss " << io.selInDiskMiss
		<< " (" << inDiskMissRatio << "%) | file-fail " << io.selFileFail
		<< " | file-ok " << io.selOk << std::endl;
	std::cout << "  disk-sel-fail: check " << io.selFailCheck << " | name " << io.selFailName
		<< " | eof " << io.selFailEof << " | type " << io.selFailType << " | open " << io.selFailOpen << std::endl;
	double busyRatio = q.busyUs > 0 ? 100.0 * (q.busyUs / 1000.0) / runMs : 0.0;
	std::cout << "  disk-queue: depth avg " << q.avgDepth << " / max " << q.maxDepth
		<< " | worker busy " << (q.busyUs / 1000) << " ms = " << busyRatio << "% (" << q.runCnt << " tasks)" << std::endl;
	double avgCompactMs = io.compactCnt > 0 ? static_cast<double>(io.compactUs) / io.compactCnt / 1000.0 : 0.0;
	std::cout << "  disk-compact: " << io.compactCnt << " runs / fail " << io.compactFail
		<< " | time " << (io.compactUs / 1000) << " ms (avg " << avgCompactMs << " ms/call)"
		<< " | file " << (io.compactBefore / 1024 / 1024) << "MB -> " << (io.compactAfter / 1024 / 1024)
		<< "MB (reclaim " << ((io.compactBefore - io.compactAfter) / 1024 / 1024) << "MB)" << std::endl;
}
#endif

// ============ 场景 ============

// QPS 档场景：数据集 300M（1:3:5；含注入池则缓慢 add 新 key 模拟数据集增长）
static void qpsScene(const char* title, double readRate) {
	std::filesystem::remove(DB_NAME);
	Controller db(MEMO_SIZE, DISK_SIZE, POOL_THREADS, DB_NAME);
	std::mt19937 rng(0x20240903);

	auto [pool, poolBytes] = buildPool(WARM_BYTES, rng);
	std::shuffle(pool.begin(), pool.end(), rng);
	std::cout << "[prewarm " << title << "] pool " << pool.size() << " keys, "
		<< (poolBytes / 1024 / 1024) << " MB, writing & flushing to disk..." << std::endl;
	warmUp(db, pool, rng);
	std::cout << "[prewarm done] load running for 30s (WRITE_INFLIGHT=" << WRITE_INFLIGHT << ")" << std::endl;

	// 注入池（写场景）：INJECT_KEYS 条缓慢新增（≈24MB），与预热 300M 合计 ≈ 324M（≈1:3.2:5），见顶部比例说明
	std::vector<PoolEntry> injectPool;
	if (readRate < 1.0) {
		std::mt19937 rng2(0x20250903);
		for (int i = 0; i < INJECT_KEYS; ++i) {
			injectPool.push_back({ "inj_" + std::to_string(i), randSizeUpdate(rng2) });
		}
	}

	// 纯读档：1:3 数据集下"全量读"= 磁盘冷读（把磁盘线程打满、QPS 失真），
	// 故纯读只打"热子集"（≤ 0.7×缓存字节，基本常驻缓存）→ 测内存命中读吞吐；
	// 冷读/磁盘读行为交给 hit-rate 2x/3x 档覆盖
	std::vector<PoolEntry> loadPool;
	if (readRate >= 1.0) {
		const long long hotLimit = MEMO_SIZE * 7 / 10;	// ~70MB < 缓存 100M
		long long hotBytes = 0;
		for (const auto& e : pool) {
			if (hotBytes >= hotLimit) break;
			loadPool.push_back(e);
			hotBytes += e.size;
		}
		std::cout << "[read-only] hot subset " << loadPool.size() << " keys / "
			<< (hotBytes / 1024 / 1024) << " MB (memory-hit reads; cold/disk reads -> hit-rate scenes)" << std::endl;
	} else {
		loadPool = pool;	// 含写场景：全数据集 300M + 缓慢注入
	}

	LoadResult r = runLoad(db, loadPool, readRate, true, RUN_SECONDS, readRate < 1.0 ? &injectPool : nullptr);
	CacheStat s = db.getStat();
	printLoad(title, r, s, RUN_SECONDS);
#if 0
	printDiskIo(db.getIoStat(), db.getQueueStat(), RUN_SECONDS);
#endif
}

// 命中率档场景：总数据 = times 倍缓存，每条大小对数正态随机，纯读按冷热分布
static void hitRateScene(int times) {
	std::filesystem::remove(DB_NAME);
	Controller db(MEMO_SIZE, DISK_SIZE, POOL_THREADS, DB_NAME);
	std::mt19937 rng(0x20240903 + times);

	auto [pool, poolBytes] = buildPool(MEMO_SIZE * times, rng, &randSizeLogNormal);
	std::shuffle(pool.begin(), pool.end(), rng);
	std::cout << "[prewarm hit-rate " << times << "x] " << pool.size() << " keys, "
		<< (poolBytes / 1024 / 1024) << " MB, writing & flushing to disk..." << std::endl;
	warmUp(db, pool, rng);
	std::cout << "[prewarm done] load running for 30s" << std::endl;

	LoadResult r = runLoad(db, pool, 1.0, true, RUN_SECONDS, nullptr);
	CacheStat s = db.getStat();
	char title[64];
	std::snprintf(title, sizeof(title), "hit-rate %dx (total %lld MB, log-normal 1K-10M)",
		times, static_cast<long long>(poolBytes / 1024 / 1024));
	printLoad(title, r, s, RUN_SECONDS);
#if 0
	printDiskIo(db.getIoStat(), db.getQueueStat(), RUN_SECONDS);
#endif
}

// ============ main ============

int main() {
	std::cout << "================== START (performance benchmark, write-inflight edition) ==================" << std::endl;
	DEFINE_DATA_TYPE(BenchData);

	auto begin = std::chrono::steady_clock::now();

	// 1. 裸接口 QPS：全读 / 读7写3 / 全写（各自重建 db，互不污染）
	// qpsScene("[1] QPS - read-only", 1.0);
	qpsScene("[2] QPS - read70/write30", 0.7);		
	// qpsScene("[3] QPS - write-only", 0.0);

	// 2. 缓存命中率对照：总数据 = 缓存 2x / 3x（200M / 300M，比例 1:2:5 / 1:3:5，磁盘 500M 内）
	// hitRateScene(2);
	// hitRateScene(3);
	// hitRateScene(4);	// 4x=400M（≈1:4:5）可选对照；跑全量会明显增加预热时长与 SSD 写入

	std::filesystem::remove(DB_NAME);	// 清理本次测试数据文件
	double totalSec = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - begin).count();
	std::cout << "================== END, total " << static_cast<long long>(totalSec)
		<< "s ==================" << std::endl;
	return 0;
}
