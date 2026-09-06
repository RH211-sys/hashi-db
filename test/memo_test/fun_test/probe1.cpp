// memo_probe1：add/del/sel 返回码最小探针（诊断用，非正式功能测试）
//
// 目的：在当前构建上直接核对"写操作执行了但返回码错位(201/202)"的疑点
//   A 段：干净流程（无淘汰）—— 逐个操作打印 code，期待值见注释
//   B 段：淘汰流程 —— 小缓存 + 大量唯一 key 持续 add，统计各返回码计数；
//         刷盘屏障后随机 sel/mod/del 读回，统计 200/202/204 分布
// 输出全 ASCII，避免控制台代码页乱码干扰判断。
#include <iostream>
#include <filesystem>
#include <random>
#include <string>
#include <vector>
#include <chrono>

#include "data_memo_level/controller/controller.h"
#include "data_memo_level/data_type.h"

struct PData {
	std::string s;
	int n;

	static std::string getClassName() { return "PData"; }
	template <class Archive>
	void serialize(Archive& ar) { ar(s, n); }
	static long long theSize(const PData& d) { return static_cast<long long>(sizeof(PData) + d.s.size()); }
};

static int g_fail = 0;

static void line(const char* name, int code, int expect) {
	bool ok = (code == expect);
	if (!ok) ++g_fail;
	std::cout << (ok ? "[ok]   " : "[FAIL] ") << name << " -> code " << code
		<< (expect >= 0 ? " (expect " + std::to_string(expect) + ")" : "") << std::endl;
}

int main() {
	std::cout << "================== PROBE1 START ==================" << std::endl;
	DEFINE_DATA_TYPE(PData);
	const std::string DB = "probe1_data.dat";

	{
		/* ============ A. 干净流程返回码 ============ */
		std::cout << "--- A. clean flow (no eviction) ---" << std::endl;
		std::filesystem::remove(DB);
		Controller db(1LL * 1024 * 1024, 64LL * 1024 * 1024, 4, DB);

		line("A add k0 (new)", db.addData("k0", PData{ "a", 0 }, true, std::chrono::seconds(0)).get(), SUCCESS);
		line("A add k0 (dup)", db.addData("k0", PData{ "b", 1 }, true, std::chrono::seconds(0)).get(), KEY_EXIST);

		auto sel0 = db.selData("k0").get();
		line("A sel k0", sel0.resCode, SUCCESS);
		if (sel0.resCode == SUCCESS && sel0.entity) {
			PData v = std::any_cast<PData>(*sel0.entity);
			std::cout << "      value ok=" << (v.s == "a" && v.n == 0) << " (" << v.s << "," << v.n << ")" << std::endl;
			if (!(v.s == "a" && v.n == 0)) ++g_fail;
		}

		line("A mod k0 (exist)", db.modData("k0", PData{ "c", 2 }).get(), SUCCESS);
		line("A mod k0 (not exist)", db.modData("k_missing", PData{ "x", 9 }).get(), FIND_FAILED);
		line("A del k0 (exist)", db.delData("k0").get(), SUCCESS);
		line("A sel k0 (after del)", db.selData("k0").get().resCode, FIND_FAILED);
		line("A del k0 (again)", db.delData("k0").get(), FIND_FAILED);
	}

	{
		/* ============ B. 淘汰流程：唯一 key 持续 add 的返回码分布 ============ */
		std::cout << "--- B. eviction churn ---" << std::endl;
		std::filesystem::remove(DB);
		Controller db(96 * 1024, 128LL * 1024 * 1024, 4, DB);	// 96KB 缓存，逼出淘汰

		const int N = 4000;
		long long cnt200 = 0, cnt201 = 0, cntOther = 0, otherCode = -1;
		std::cout << "adding " << N << " unique keys..." << std::endl;
		for (int i = 0; i < N; ++i) {
			std::string key = "p_" + std::to_string(i);
			int code = db.addData(key, PData{ "v", i }, true, std::chrono::seconds(0)).get();
			if (code == SUCCESS) ++cnt200;
			else if (code == KEY_EXIST) { ++cnt201; if (otherCode < 0) otherCode = code; }
			else { ++cntOther; otherCode = code; }
		}
		std::cout << "B add: SUCCESS=" << cnt200 << " KEY_EXIST(201)=" << cnt201
			<< " other=" << cntOther;
		if (otherCode >= 0) std::cout << " (first other=" << otherCode << ")";
		std::cout << std::endl;
		if (cnt201 + cntOther > 0) ++g_fail;

		// 刷盘屏障：淘汰 fire-and-forget 的落盘全部完成后再读
		line("B flushDisk barrier", db.flushDisk().get(), SUCCESS);

		// 随机抽 200 个 key 读回（大部分应已被淘汰、走磁盘回填）
		std::mt19937 rng(42);
		long long s200 = 0, s202 = 0, s204 = 0, sOther = 0;
		int valueMismatch = 0, sample = 0;
		for (int t = 0; t < 200; ++t) {
			int i = static_cast<int>(rng() % N);
			auto r = db.selData("p_" + std::to_string(i)).get();
			if (r.resCode == SUCCESS) {
				++s200;
				if (r.entity) {
					PData v = std::any_cast<PData>(*r.entity);
					if (v.n != i) ++valueMismatch;
				}
			}
			else if (r.resCode == FIND_FAILED) ++s202;
			else if (r.resCode == EXPIRED) ++s204;
			else ++sOther;
			++sample;
		}
		std::cout << "B sel sample " << sample << ": SUCCESS=" << s200 << " FIND_FAILED=" << s202
			<< " EXPIRED=" << s204 << " other=" << sOther << " valueMismatch=" << valueMismatch << std::endl;
		if (sOther + valueMismatch > 0) ++g_fail;

		// 改/删各抽 100 个（含必然已淘汰回填的 key）
		long long m200 = 0, mOther = 0, mOtherCode = -1;
		for (int t = 0; t < 100; ++t) {
			int i = static_cast<int>(rng() % N);
			int c = db.modData("p_" + std::to_string(i), PData{ "m", i }).get();
			if (c == SUCCESS) ++m200; else { ++mOther; mOtherCode = c; }
		}
		std::cout << "B mod 100: SUCCESS=" << m200 << " other=" << mOther;
		if (mOtherCode >= 0) std::cout << " (first=" << mOtherCode << ")";
		std::cout << std::endl;

		long long d200 = 0, d202 = 0, dOther = 0, dOtherCode = -1;
		for (int t = 0; t < 100; ++t) {
			int i = static_cast<int>(rng() % N);
			int c = db.delData("p_" + std::to_string(i)).get();
			if (c == SUCCESS) ++d200;
			else if (c == FIND_FAILED) ++d202;
			else { ++dOther; dOtherCode = c; }
		}
		std::cout << "B del 100: SUCCESS=" << d200 << " FIND_FAILED=" << d202 << " other=" << dOther;
		if (dOtherCode >= 0) std::cout << " (first=" << dOtherCode << ")";
		std::cout << std::endl;
		if (dOther > 0) ++g_fail;

		line("B reWrite", db.reWrite().get(), SUCCESS);
		std::cout << "B cache stat: hit=" << db.getStat().hit << " miss=" << db.getStat().miss
			<< " evictCalls=" << db.getStat().evictCnt << std::endl;
	}

	std::cout << "================== PROBE1 END, fail=" << g_fail << " ==================" << std::endl;
	std::filesystem::remove(DB);
	return g_fail ? 1 : 0;
}
