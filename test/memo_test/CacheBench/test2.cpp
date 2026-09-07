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

#include "data_memo_level/controller/controller.h"
#include "data_memo_level/data_type.h"

/*
	性能测试（memo_test2）：测存储层在固定时长窗口内的接口吞吐与延迟分布
	配置：缓存 100M / 磁盘 500M / 读线程池 4 / 数据文件 test2_data.dat（与 test1 同命名风格）

	测试项：
	1. 裸接口 QPS：全读 / 读7写3 / 全写 三种负载各跑 30s，记录 QPS + 平均 / P50 / P99 / P999 延迟
	2. 单条数据 1K~10M 随机大小，内容随机填充
	3. key 冷热分布：20% key（池前 1/5）承担 80% 请求
	4. 缓存命中率：总数据量 2x / 3x / 4x 缓存（200M/300M/400M，每条大小对数正态随机），记录命中次数 / 总请求
	5. 淘汰对写吞吐的影响：CacheStat 给出 evict 次数 / 条目数 / 总耗时，结合写延迟尾部分布观察

	设计说明：
	- 命中率与淘汰数据来自 Cache 的只读统计接口（getStat，原子计数），黑盒无法区分缓存命中与磁盘读回
	- 磁盘容量有界（500M）：写负载以 modData 热更新为主，另有注入线程限速 add 新 key 模拟"新增数据流"，
	  持续制造缓存超限淘汰压力；无界 add 会溢出磁盘
	- 单条大小档位分布而非线性均匀：线性均匀均值 ~5M，100M 缓存只能装约 20 条，key 数与冷热切分全部失真；
	  档位分布保留 1K~10M 全跨度同时把均值压到 30 万字节量级（缓存能装几百 key）
	- 命中率档每条大小对数正态（log10 均值 100K、σ 0.5，覆盖 1K~10M）：真实负载大小混合，
	  key 数仍约千级可支撑冷热切分；命中率下限 = 淘汰目标 0.9×100M / 总量，miss 回填保活热 key
	  与淘汰对大小的偏置会把命中率推高，观测偏离下限的幅度即这两重机制的强度信号
	- 预热结束 flushDisk().get() 做刷盘屏障：flushDisk 排在磁盘线程队尾，返回时此前 evict
	  fire-and-forget 提交的刷盘任务已全部落盘，避免"脏数据未落盘就被读 miss"的计数空洞
	- 预热前 shuffle key 池：否则按序 add 时最先 add 的 key（池前 20% = 热区）最先被挤出缓存，
	  热区系统性全部 miss，命中率数字失真
	- 延迟样本蓄水池采样（每线程上限 20 万）：30s 高 QPS 全量样本会占数百 MB 内存，抽样不影响分位估计
	- 运行时长 ≈ 6 场景 × (预热 + 30s)，命中率档预热需把 400M 数据写入并落盘，整体约数分钟
*/

// ============ 配置 ============

constexpr long long MEMO_SIZE = 100LL * 1024 * 1024;	// 缓存 100M（超限触发全局采样淘汰）
constexpr long long DISK_SIZE = 500LL * 1024 * 1024;	// 磁盘 500M（命中率档最大 4x = 400M 数据需落盘）
constexpr int POOL_THREADS = 4;							// 读线程池线程数
const std::string DB_NAME = "test2_data.dat";			// 数据库文件
constexpr long long WARM_BYTES = 95LL * 1024 * 1024;	// QPS 档预热池 1M（接近缓存上限但预热不触发淘汰；写波动即超限）
constexpr double RUN_SECONDS = 30.0;					// 每场景固定时长
constexpr int WORKERS = 8;								// 压测线程数（读池 4 线程 + 写线程 1，8 个发起者足够压满）
constexpr int INJECT_MS = 30;							// 注入线程 add 间隔（约 33 条/s，30s 内磁盘余量内）
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

// 单条大小档位分布，覆盖 1K~10M 全跨度：
//   70% [1K, 16K]   / 25% [16K, 1M]   / 5% [1M, 10M]
//   高概率小数据保证 key 数量；低概率大数据制造大小波动（mod 替换大实体即触发淘汰）
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

// 数据大小对数正态（数量级 log10 服从正态）：大小跨 1K~10M 四个数量级，线性正态会大量越界
// 被钳成两堆边界尖峰，对数尺度正态自然覆盖全区间——log10 均值 5.0（中位 100K）、σ 0.5，
// 期望 ≈ 194K/条（与 QPS 档同量级，缓存可装约千 key 支撑冷热切分）；[1K, 10M] = 均值 ±4σ，
// 越界概率 ≈ 6e-5/尾，钳位仅作防御（命中率档用，观察大小混合下的淘汰偏置与命中率）
static long long randSizeLogNormal(std::mt19937& rng) {
	thread_local std::normal_distribution<double> dist(5.0, 0.5);	// 分布有内部状态，每线程独立
	double lg = dist(rng);
	if (lg < 3.0) lg = 3.0;	// 钳位 1K
	if (lg > 7.0) lg = 7.0;	// 钳位 10M
	return static_cast<long long>(std::pow(10.0, lg));
}

// 构造随机内容的实体（内容不跨进程使用，字节序无约束）
static BenchData makeData(long long size, std::mt19937& rng) {
	BenchData b;
	b.size = size;
	b.data.resize(size);
	// 8 字节块走 mt19937_64（快），剩余字节走 rng 补齐
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

// 按字节预算建 key 池：逐条按大小生成器取样累加，超过预算停止
// sizeGen：每条数据大小的取样函数（默认档位分布；命中率档传对数正态）
// 返回 <key 池, 实际总字节>（预算为下限，末条可能略超预算）
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
	// 刷盘屏障：flushDisk 排在磁盘线程队尾，get() 返回时 evict 提交的刷盘任务已全部落盘
	db.flushDisk().get();
}

// ============ 延迟采样与压测循环 ============

// 延迟样本蓄水池（单位 ns）：30s 高 QPS 全量样本占内存过大，水位线抽样保持总体分布
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
		size_t j = static_cast<size_t>(rng()) % seen;	// 蓄水池替换判定（% 取模偏差可忽略）
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

// 压力循环：WORKERS 个测试线程在 seconds 秒内循环 submit + get()（端到端延迟 = 发起请求到拿到结果）
// readRate：读概率，其余为写（写 = modData 热更新：磁盘容量有界，无界 add 会溢出磁盘；
//   modData 与 addData 走同一条写线程路径，替换大实体同样触发淘汰，是容量有界系统的真实稳态写形态）
// hotSpot：请求分布 20% key（池前 1/5）承担 80% 请求
// injectPool 非空：注入线程每 INJECT_MS 顺次 add 一条新 key（模拟新增数据流，持续制造淘汰压力）
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
			while (!stop.load(std::memory_order_relaxed)) {
				bool isRead = unit(rng) < readRate;
				size_t idx = pickIdx(rng);
				auto t0 = std::chrono::steady_clock::now();
				if (isRead) {
					// 只测时序不校验值：读结果体拿 resCode 即弃（命中/磁盘读回都算一次查询）
					auto res = db.selData(pool[idx].key).get();
					(void)res;
				} else {
					long long sz = randSizeTiered(rng);
					db.modData(pool[idx].key, std::move(makeData(sz, rng))).get();
				}
				long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - t0).count();
				samples[w].add(ns, rng);
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

	// 汇总：合并蓄水池样本排序取分位（8 线程 × 20 万 ≤ 160 万，可接受）
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

// 输出：只保留 QPS / P99 / 缓存命中率（探测性明细已移到其他分支）
static void printLoad(const char* title, const LoadResult& r, const CacheStat& s, double seconds) {
	long long seen = s.hit + s.miss;
	double hitRate = seen > 0 ? 100.0 * s.hit / seen : 0.0;
	std::cout << "===== " << title << " (duration " << seconds << "s) =====" << std::endl;
	std::cout << "  QPS " << static_cast<long long>(r.qps)
		<< " | P99 " << static_cast<long long>(r.p99Us) << " us"
		<< " | hit rate " << hitRate << "%" << std::endl;
}

// ============ 磁盘 IO 阶段占比输出（探测，已用 /* */ 注释：明细移到其他分支，展开即可恢复）============
/*
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
*/	// 探测输出注释结束

// ============ 场景 ============

// QPS 档场景：预热池 95M（含注入池则边测边 add 新 key 制造淘汰压力）
static void qpsScene(const char* title, double readRate) {
	std::filesystem::remove(DB_NAME);
	Controller db(MEMO_SIZE, DISK_SIZE, POOL_THREADS, DB_NAME);
	std::mt19937 rng(0x20240903);

	auto [pool, poolBytes] = buildPool(WARM_BYTES, rng);
	std::shuffle(pool.begin(), pool.end(), rng);
	std::cout << "[prewarm " << title << "] pool " << pool.size() << " keys, "
		<< (poolBytes / 1024 / 1024) << " MB, writing & flushing to disk..." << std::endl;
	warmUp(db, pool, rng);
	std::cout << "[prewarm done] load running for 30s" << std::endl;

	// 注入池（写场景）：约 800 条新 key ≈ 24s 注入量（磁盘余量 500M-95M 内；跑不完自动停，无影响）
	std::vector<PoolEntry> injectPool;
	if (readRate < 1.0) {
		std::mt19937 rng2(0x20250903);
		for (int i = 0; i < 800; ++i) {
			injectPool.push_back({ "inj_" + std::to_string(i), randSizeTiered(rng2) });
		}
	}

	LoadResult r = runLoad(db, pool, readRate, true, RUN_SECONDS, readRate < 1.0 ? &injectPool : nullptr);
	CacheStat s = db.getStat();
	printLoad(title, r, s, RUN_SECONDS);
	// printDiskIo(db.getIoStat(), db.getQueueStat(), RUN_SECONDS);	// 探测输出（已注释）
}

// 命中率档场景：总数据 = times 倍缓存，每条大小对数正态随机（1K~10M），纯读按冷热分布 30s
static void hitRateScene(int times) {
	std::filesystem::remove(DB_NAME);
	Controller db(MEMO_SIZE, DISK_SIZE, POOL_THREADS, DB_NAME);
	std::mt19937 rng(0x20240903 + times);

	// 命中率预期：稳态驻留 ≈ evict 目标 90M（容量有界必然淘汰），90M/总数据量是纯容量下限；
	// 实测高于下限属正常——miss 回填刷新 updateTime，淘汰删最旧 → 高频 key 经回填保活，
	// 冷热 80/20 下热区驻留率显著偏高；大小随机后再叠加淘汰对大小的偏置（偏删大 key 时
	// 同字节驻留 key 更多，按请求计命中率更高）。偏离下限的幅度 = 这两重机制的强度信号
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
	// printDiskIo(db.getIoStat(), db.getQueueStat(), RUN_SECONDS);	// 探测输出（已注释）
}

// ============ main ============

int main() {
	std::cout << "================== START (performance benchmark) ==================" << std::endl;
	DEFINE_DATA_TYPE(BenchData);

	auto begin = std::chrono::steady_clock::now();

	// 1. 裸接口 QPS：全读 / 读7写3 / 全写（各自重建 db，互不污染）
	qpsScene("[1] QPS - read-only", 1.0);
	// qpsScene("[2] QPS - read70/write30", 0.7);
	// qpsScene("[3] QPS - write-only", 0.0);

	// 2. 缓存命中率：总数据 2x / 3x / 4x 缓存
	//hitRateScene(2);
	//hitRateScene(3);
	//hitRateScene(4);

	std::filesystem::remove(DB_NAME);	// 清理本次测试数据文件
	double totalSec = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - begin).count();
	std::cout << "================== END, total " << static_cast<long long>(totalSec)
		<< "s ==================" << std::endl;
	return 0;
}
