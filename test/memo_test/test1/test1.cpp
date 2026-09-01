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
	覆盖：新增/查询/修改/删除/过期/LRU 淘汰/修改后淘汰落盘/持久化/重写
	运行：VS 中启动 memo_test1；断言失败会打印 [FAIL] 并计数
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
	std::cout << "================== 开始 ===============" << std::endl;
	DEFINE_DATA_TYPE(User);
	DEFINE_DATA_TYPE(Score);

	// 清理上次测试残留的数据文件
	std::filesystem::remove("test1_data.dat");

	{
		// 建库：缓存 64KB（超限触发 LRU 淘汰），磁盘 64MB，读线程池 4（预留），数据文件 test1_data.dat
		Controller db(64 * 1024, 64 * 1024 * 1024, 4, "test1_data.dat");

		// ===== 1. 新增（INSERT）=====
		std::cout << "===== 1. INSERT =====" << std::endl;
		CHECK_CODE(db.addData("user1", User{"alice", 25}, true, std::chrono::seconds(0)), SUCCESS, "add user1");
		CHECK_CODE(db.addData("score", Score{100}, true, std::chrono::seconds(0)), SUCCESS, "add score");
		// 临时数据：1 秒后过期
		CHECK_CODE(db.addData("tmp", User{"tmp", 0}, false, std::chrono::seconds(1)), SUCCESS, "add tmp");
		// 重复新增：缓存或磁盘已存在 → 冲突
		CHECK_CODE(db.addData("user1", User{"bob", 30}, true, std::chrono::seconds(0)), KEY_EXIST, "add user1 dup");

		// ===== 2. 查询（SELECT）=====
		std::cout << "===== 2. SELECT =====" << std::endl;
		std::any res;
		int code = db.selData("user1", res);
		CHECK_CODE(code, SUCCESS, "sel user1");
		if (code == SUCCESS) {
			User u = std::any_cast<User>(res);
			CHECK(u.name == "alice" && u.age == 25, "sel user1 value ok");
		}
		// 查不存在的变量
		CHECK_CODE(db.selData("not_exist", res), FIND_FAILED, "sel not_exist");

		// ===== 3. 修改（MODIFY）=====
		std::cout << "===== 3. MODIFY =====" << std::endl;
		CHECK_CODE(db.modData("user1", User{"alice", 26}), SUCCESS, "mod user1");
		code = db.selData("user1", res);
		CHECK_CODE(code, SUCCESS, "sel user1 after mod");
		if (code == SUCCESS) {
			User u = std::any_cast<User>(res);
			CHECK(u.name == "alice" && u.age == 26, "mod value ok");
		}
		// 修改不存在的变量
		CHECK_CODE(db.modData("not_exist", User{"x", 0}), FIND_FAILED, "mod not_exist");

		// ===== 4. 删除（DELETE）=====
		std::cout << "===== 4. DELETE =====" << std::endl;
		CHECK_CODE(db.delData("score"), SUCCESS, "del score");
		CHECK_CODE(db.delData("score"), FIND_FAILED, "del score dup");
		CHECK_CODE(db.selData("score", res), FIND_FAILED, "sel score after del");

		// ===== 5. 过期（EXPIRED）：命中缓存的过期数据 → 删缓存 + 提交磁盘删除 + 返回 204 =====
		std::cout << "===== 5. EXPIRED =====" << std::endl;
		std::this_thread::sleep_for(std::chrono::milliseconds(1200));	// 等 tmp 过期
		CHECK_CODE(db.selData("tmp", res), EXPIRED, "sel tmp expired");
		std::this_thread::sleep_for(std::chrono::milliseconds(500));	// 等磁盘删除任务执行
		CHECK_CODE(db.selData("tmp", res), FIND_FAILED, "sel tmp after expired del");

		// ===== 6. LRU 淘汰：挤满缓存触发淘汰，被挤出数据从磁盘回填且值正确 =====
		std::cout << "===== 6. LRU EVICT =====" << std::endl;
		const int BULK_NUM = 3000;	// 64KB 缓存约可放 1300 条，3000 条足以触发多轮淘汰
		for (int i = 0; i < BULK_NUM; ++i) {
			db.addData("bulk" + std::to_string(i), User{"u" + std::to_string(i), i}, true, std::chrono::seconds(0));
		}
		// 同步等待磁盘任务队列排空（flushDisk 同步等待，前面的淘汰刷盘必然已执行完）
		CHECK_CODE(db.flushDisk(), SUCCESS, "flushDisk after bulk");
		// 早期数据肯定已被挤出缓存：从磁盘回填，值应正确
		for (int i = 10; i <= 1000; i += 10) {
			std::string key = "bulk" + std::to_string(i);
			code = db.selData(key, res);
			if (code == SUCCESS) {
				User u = std::any_cast<User>(res);
				CHECK(u.name == "u" + std::to_string(i) && u.age == i, "LRU refill " + key);
			} else {
				CHECK(false, "LRU refill " + key + " (code=" + std::to_string(code) + ")");
			}
		}

		// ===== 7. 修改后淘汰：脏数据落盘，挤出后回填应拿到新值 =====
		std::cout << "===== 7. MODIFY+EVICT =====" << std::endl;
		CHECK_CODE(db.modData("bulk1700", User{"new", 999}), SUCCESS, "mod bulk1700");
		for (int i = BULK_NUM; i < BULK_NUM + 2000; ++i) {	// 加 2000 条，把 bulk1700 挤出缓存
			db.addData("bulk" + std::to_string(i), User{"u" + std::to_string(i), i}, true, std::chrono::seconds(0));
		}
		CHECK_CODE(db.flushDisk(), SUCCESS, "flushDisk after mod");
		code = db.selData("bulk1700", res);
		CHECK_CODE(code, SUCCESS, "sel bulk1700 after evict");
		if (code == SUCCESS) {
			User u = std::any_cast<User>(res);
			CHECK(u.name == "new" && u.age == 999, "mod+evict refill new value");
		}

		// ===== 8. 持久化（PERSIST / FLUSH / REWRITE）=====
		std::cout << "===== 8. PERSIST =====" << std::endl;
		// persisVar 持久化缓存中的数据（无 Val 版从缓存取，user1 已被 LRU 挤出缓存，这里用缓存中存在的）
		CHECK_CODE(db.persisVar("bulk4999"), SUCCESS, "persisVar bulk4999");
		CHECK_CODE(db.persisAll(), SUCCESS, "persisAll");
		CHECK_CODE(db.flushDisk(), SUCCESS, "flushDisk");
		CHECK_CODE(db.reWrite(), SUCCESS, "reWrite");
		// 重写后数据仍完整（紧凑写入后偏移已更新）
		code = db.selData("user1", res);
		CHECK_CODE(code, SUCCESS, "sel user1 after reWrite");
		if (code == SUCCESS) {
			User u = std::any_cast<User>(res);
			CHECK(u.name == "alice" && u.age == 26, "reWrite value ok");
		}
		// 重写后文件应存在且非空
		CHECK(std::filesystem::file_size("test1_data.dat") > 0, "reWrite file non-empty");

		// ===== 汇总 =====
		std::cout << "\n===== total " << g_total << " asserts, failed " << g_fail << " =====" << std::endl;
	}	// db 析构：磁盘线程处理完队列任务后退出

	// 清理数据文件
	std::filesystem::remove("test1_data.dat");
	return g_fail ? 1 : 0;
}
