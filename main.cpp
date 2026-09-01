#include <iostream>
#include <any>
#include <chrono>
#include <thread>

#include "data_memo_level/controller/controller.h"
#include "data_memo_level/data_type.h"

/*
	演示用数据类型：用户自定义类型必须实现（见 data_type.h 说明）
	1. 获取类名 getClassName（静态）
	2. 序列化模板 serialize（cereal 按字段自动打包/拆包）
	3. 计算对象大小 theSize（静态，用于 LRU 分数与内存统计）
	定义后通过 DEFINE_DATA_TYPE 注册序列化/反序列化函数
*/

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


int main() {
	DEFINE_DATA_TYPE(User);
	DEFINE_DATA_TYPE(Score);
	// 建库：缓存 1MB（超限触发 LRU 淘汰），磁盘 64MB，读线程池 4（预留），数据文件 myDB.dat
	Controller db(1024 * 1024, 64 * 1024 * 1024, 4, "myDB.dat");

	// ===== 新增（INSERT）=====
	std::cout << "add user1     : " << db.addData("user1", User{"alice", 25}, true, std::chrono::seconds(0)) << std::endl;
	std::cout << "add score     : " << db.addData("score", Score{100}, true, std::chrono::seconds(0)) << std::endl;

	// 临时数据：1 秒后过期
	std::cout << "add tmp       : " << db.addData("tmp", User{"tmp", 0}, false, std::chrono::seconds(1)) << std::endl;

	// 重复新增：冲突（缓存或磁盘已存在）
	std::cout << "add user1 dup : " << db.addData("user1", User{"bob", 30}, true, std::chrono::seconds(0)) << std::endl;

	// ===== 查询（SELECT）=====
	std::any res;
	int code = db.selData("user1", res);
	User u = std::any_cast<User>(res);
	std::cout << "sel user1     : " << code << " = " << u.name << ", " << u.age << std::endl;

	// 查不存在的变量
	code = db.selData("not_exist", res);
	std::cout << "sel not_exist : " << code << std::endl;

	// ===== 修改（MODIFY）=====
	std::cout << "mod user1     : " << db.modData("user1", User{"alice", 26}) << std::endl;
	code = db.selData("user1", res);
	u = std::any_cast<User>(res);
	std::cout << "sel user1     : " << code << " = " << u.name << ", " << u.age << std::endl;

	// 修改不存在的变量 
	std::cout << "mod not_exist : " << db.modData("not_exist", User{"x", 0}) << std::endl;

	// ===== 过期（EXPIRED）=====
	std::this_thread::sleep_for(std::chrono::milliseconds(1200));	// 等 tmp 过期
	code = db.selData("tmp", res);
	std::cout << "sel tmp       : " << code << " (204 = 已过期，自动清理磁盘记录)" << std::endl;

	// ===== 删除（DELETE）=====
	std::cout << "del score     : " << db.delData("score") << std::endl;
	std::cout << "del score dup : " << db.delData("score") << std::endl;	// 已删除，返回未找到
	std::cout << "sel score     : " << db.selData("score", res) << std::endl;

	// ===== 持久化（PERSIST / FLUSH / REWRITE）=====
	std::cout << "persisAll     : " << db.persisAll() << std::endl;
	std::cout << "flushDisk     : " << db.flushDisk() << std::endl;
	std::cout << "reWrite       : " << db.reWrite() << std::endl;

	return 0;
}
