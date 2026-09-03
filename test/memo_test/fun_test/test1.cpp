#include <iostream>
#include <any>
#include <chrono>
#include <thread>
#include <string>
#include <filesystem>

#include "data_memo_level/controller/controller.h"
#include "data_memo_level/data_type.h"

/*
	功能性测试：验证所有接口 + 所有策略
	覆盖：新增/查询/修改/删除/过期/全局采样淘汰/修改后淘汰落盘/持久化/重写
	运行：VS 中启动 memo_test1；断言失败会打印 [FAIL] 并计数
	说明：存储层异步返回 future，本测试对每个调用点 get() 同步等待结果（模拟上层需要结果的场景）
*/

// ============ 测试用数据类型（需实现 getClassName / serialize / theSize，见 data_type.h） ============

struct User {
	std::string name;
	int age;

	static std::string getClassName() { return "User"; }
	template <class Archive>
	void serialize(Archive& ar) { ar(name, age); }
	static long long theSize(const User& u) { return sizeof(User) + u.name.size(); }
};

struct Score {
	long long score;

	static std::string getClassName() { return "Score"; }
	template <class Archive>
	void serialize(Archive& ar) { ar(score); }
	static long long theSize(const Score& s) { return sizeof(s); }
};

// ============ 断言辅助 ============

static int g_fail = 0;
static int g_total = 0;

#define CHECK(cond, name) do { \
	++g_total; \
	if (cond) std::cout << "[PASS] " << name << std::endl; \
	else { std::cout << "[FAIL] " << name << std::endl; ++g_fail; } \
} while (0)

#define CHECK_CODE(actual, expect, name) CHECK((actual) == (expect), \
	std::string(name) + " (code=" + std::to_string(actual) + ", expect=" + std::to_string(expect) + ")")

int main() {
	std::cout << "================== START ===============" << std::endl;
	DEFINE_DATA_TYPE(User);
	DEFINE_DATA_TYPE(Score);

	// 清理上次测试残留的数据文件
	std::filesystem::remove("test1_data.dat");

	{
		// 建库：缓存 64KB（超限触发全局采样淘汰），磁盘 64MB，读线程池 4，数据文件 test1_data.dat
		Controller db(64 * 1024, 64 * 1024 * 1024, 4, "test1_data.dat");

		// ===== 1. 新增（INSERT）=====
		std::cout << "===== 1. INSERT =====" << std::endl;
		CHECK_CODE(db.addData("user1", User{"alice", 25}, true, std::chrono::seconds(0)).get(), SUCCESS, "add user1");
		CHECK_CODE(db.addData("score", Score{100}, true, std::chrono::seconds(0)).get(), SUCCESS, "add score");
		// 临时数据：1 秒后过期
		CHECK_CODE(db.addData("tmp", User{"tmp", 0}, false, std::chrono::seconds(1)).get(), SUCCESS, "add tmp");
		// 重复新增：缓存或磁盘已存在 → 冲突
		CHECK_CODE(db.addData("user1", User{"bob", 30}, true, std::chrono::seconds(0)).get(), KEY_EXIST, "add user1 dup");

		// ===== 2. 查询（SELECT）=====
		std::cout << "===== 2. SELECT =====" << std::endl;
		auto sel = db.selData("user1").get();	// 查询结果体：resCode + 实体指针（shared_ptr，与缓存共享同一实体）
		CHECK_CODE(sel.resCode, SUCCESS, "sel user1");
		if (sel.resCode == SUCCESS) {
			User u = std::any_cast<User>(*sel.entity);	// 实体指针解引用后按注册类型还原
			CHECK(u.name == "alice" && u.age == 25, "sel user1 value ok");
		}
		// 查不存在的变量
		CHECK_CODE(db.selData("not_exist").get().resCode, FIND_FAILED, "sel not_exist");

		// ===== 3. 修改（MODIFY）=====
		std::cout << "===== 3. MODIFY =====" << std::endl;
		CHECK_CODE(db.modData("user1", User{"alice", 26}).get(), SUCCESS, "mod user1");
		sel = db.selData("user1").get();
		CHECK_CODE(sel.resCode, SUCCESS, "sel user1 after mod");
		if (sel.resCode == SUCCESS) {
			User u = std::any_cast<User>(*sel.entity);
			CHECK(u.name == "alice" && u.age == 26, "mod value ok");
		}
		// 修改不存在的变量
		CHECK_CODE(db.modData("not_exist", User{"x", 0}).get(), FIND_FAILED, "mod not_exist");

		// ===== 4. 删除（DELETE）=====
		std::cout << "===== 4. DELETE =====" << std::endl;
		CHECK_CODE(db.delData("score").get(), SUCCESS, "del score");
		CHECK_CODE(db.delData("score").get(), FIND_FAILED, "del score dup");
		CHECK_CODE(db.selData("score").get().resCode, FIND_FAILED, "sel score after del");

		// ===== 5. 过期（EXPIRED）：命中缓存的过期数据 → 删缓存 + 提交磁盘删除 + 返回 204 =====
		std::cout << "===== 5. EXPIRED =====" << std::endl;
		std::this_thread::sleep_for(std::chrono::milliseconds(1200));	// 等 tmp 过期
		CHECK_CODE(db.selData("tmp").get().resCode, EXPIRED, "sel tmp expired");
		std::this_thread::sleep_for(std::chrono::milliseconds(500));	// 等清理任务（写线程删缓存 + 磁盘线程删索引）执行
		CHECK_CODE(db.selData("tmp").get().resCode, FIND_FAILED, "sel tmp after expired del");

		// ===== 6. 全局采样淘汰：挤满缓存触发淘汰，被挤出数据从磁盘回填且值正确 =====
		std::cout << "===== 6. EVICT（全局采样）=====" << std::endl;
		const int BULK_NUM = 3000;	// 64KB 缓存约可放 1300 条，3000 条足以触发多轮淘汰
		for (int i = 0; i < BULK_NUM; ++i) {
			CHECK_CODE(db.addData("bulk" + std::to_string(i), User{"u" + std::to_string(i), i}, true, std::chrono::seconds(0)).get(), SUCCESS, "add bulk" + std::to_string(i));
		}
		// 同步等待磁盘任务队列排空（flushDisk 同步等待，前面的淘汰刷盘必然已执行完）
		CHECK_CODE(db.flushDisk().get(), SUCCESS, "flushDisk after bulk");
		// 早期数据大概率已被挤出缓存（全局采样）：从磁盘回填，值应正确；仍在缓存则直接命中
		for (int i = 10; i <= 1000; i += 10) {
			std::string key = "bulk" + std::to_string(i);
			sel = db.selData(key).get();
			if (sel.resCode == SUCCESS) {
				User u = std::any_cast<User>(*sel.entity);
				CHECK(u.name == "u" + std::to_string(i) && u.age == i, "evict refill " + key);
			} else {
				CHECK(false, "evict refill " + key + " (code=" + std::to_string(sel.resCode) + ")");
			}
		}

		// ===== 7. 修改后淘汰：脏数据落盘，挤出后回填应拿到新值 =====
		std::cout << "===== 7. MODIFY + EVICT =====" << std::endl;
		CHECK_CODE(db.modData("bulk1700", User{"new", 999}).get(), SUCCESS, "mod bulk1700");
		for (int i = BULK_NUM; i < BULK_NUM + 2000; ++i) {	// 加 2000 条，把 bulk1700 挤出缓存
			CHECK_CODE(db.addData("bulk" + std::to_string(i), User{"u" + std::to_string(i), i}, true, std::chrono::seconds(0)).get(), SUCCESS, "add bulk" + std::to_string(i));
		}
		CHECK_CODE(db.flushDisk().get(), SUCCESS, "flushDisk after mod");
		sel = db.selData("bulk1700").get();
		CHECK_CODE(sel.resCode, SUCCESS, "sel bulk1700 after evict");
		if (sel.resCode == SUCCESS) {
			User u = std::any_cast<User>(*sel.entity);
			CHECK(u.name == "new" && u.age == 999, "mod+evict refill new value");
		}

		// ===== 8. 持久化（PERSIST / FLUSH / REWRITE）=====
		std::cout << "===== 8. PERSIST =====" << std::endl;
		// persisVar 持久化缓存中的数据（无 Val 版从缓存取）：modData 命中磁盘回填缓存并同步等待，保证该 key 在缓存
		CHECK_CODE(db.modData("bulk4999", User{"u4999", 4999}).get(), SUCCESS, "mod bulk4999 (refill to cache)");
		CHECK_CODE(db.persisVar("bulk4999").get(), SUCCESS, "persisVar bulk4999");
		CHECK_CODE(db.persisAll().get(), SUCCESS, "persisAll");
		CHECK_CODE(db.flushDisk().get(), SUCCESS, "flushDisk");
		CHECK_CODE(db.reWrite().get(), SUCCESS, "reWrite");
		// 重写后数据仍完整（紧凑写入后偏移已更新）
		sel = db.selData("user1").get();
		CHECK_CODE(sel.resCode, SUCCESS, "sel user1 after reWrite");
		if (sel.resCode == SUCCESS) {
			User u = std::any_cast<User>(*sel.entity);
			CHECK(u.name == "alice" && u.age == 26, "reWrite value ok");
		}
		// 重写后文件应存在且非空
		CHECK(std::filesystem::file_size("test1_data.dat") > 0, "reWrite file non-empty");

		// ===== 汇总 =====
		std::cout << "\n===== total " << g_total << " asserts, failed " << g_fail << " =====" << std::endl;
	}	// db 析构：各线程处理完队列任务后退出

	// 清理数据文件
	std::filesystem::remove("test1_data.dat");
	return g_fail ? 1 : 0;
}
