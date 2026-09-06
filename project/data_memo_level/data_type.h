#pragma once
#ifndef _DATA_TYPE_H_
#define _DATA_TYPE_H_

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include <unordered_set>
#include <string>
#include <any>
#include <chrono>
#include <atomic>
#include <ostream>
#include <sstream>
#include <memory>
#include "protocol.h"

/*
	该文件通过#include用户自定义的数据类型，只需include
	用户需要专门创建一个文件A，定义自定义类，然后在该文件中include文件A
	自定义类，必要的方法：
	1. 获取数据类型的类名(getClassName)之类的函数
	2. 序列化成员模板 serialize(Archive&)，cereal 按字段自动打包/拆包
	3. 用户在服务器需要定义好计算对象大小的成员函数，以提高处理速度，函数名是theSize
*/

inline std::unordered_set<std::string> totalType;

/* ========== 类型注册表 ========== */
// 序列化函数：把 any 里的对象直接写入调用方提供的输出流（写盘用）
// 目标流由磁盘模块指向"定长头 + 实体"的连续记录缓冲 → 序列化零中间拷贝、一次 write 落盘
using SerializeFunc = void(*)(const std::any& obj, std::ostream& os);
// 反序列化函数：把字节还原成对象，包进 any（读盘用）
using DeserializeFunc = std::any(*)(const std::vector<char>& bytes);

// 类型注册表：typeName → {序列化函数, 反序列化函数}，程序启动注册，运行期只读
inline std::unordered_map<std::string, std::pair<SerializeFunc, DeserializeFunc>> typeReg;

// 序列化：cereal 把对象直接写入调用方输出流（连续记录缓冲），免 stringstream 中间拷贝
template <typename T>
void toBytes(const std::any& obj, std::ostream& os) {
    cereal::BinaryOutputArchive ar(os);
    ar(std::any_cast<const T&>(obj));
}

// 反序列化：从字节还原对象，包进 any
template <typename T>
std::any fromBytes(const std::vector<char>& bytes) {
    std::stringstream ss(std::string(bytes.begin(), bytes.end()));
    cereal::BinaryInputArchive ar(ss);
    T obj;
    ar(obj);
    return obj;
}

/*
	注册数据类型的方法
	参数：数据类型名称
	功能：向totalType中添加该类型 + 注册其序列化/反序列化函数
	注意：自定义类必须提供成员模板 template <class Archive> void serialize(Archive&)
*/
#define DEFINE_DATA_TYPE(name) \
    totalType.insert(#name); \
    typeReg.emplace(#name, std::make_pair(toBytes<name>, fromBytes<name>))

/*
	Val：缓存条目值结构（存放在 cache_db 的 value 中）
	updateTime：缓存条目"热度"近似——最近一次读命中/插入时刻（µs since epoch，relaxed 原子）。
		- 读命中刷新（relaxed store，消除原共享锁内非原子写的多读者数据竞争）
		- addData 置 now；selData 磁盘回填沿用磁盘记录里的时间（不保活）；modData 回填保持默认 0（视为最冷）
		- modData 命中不刷新（写可能是冷数据）
		- evictScore 用它算"距上次访问间隔"分档；落盘时写入磁盘记录（内容与改动前一致）
	其余字段在表锁/记录锁保护下读写；Val 含原子成员，拷贝/移动均自定义（原子字段 relaxed 取值），
	淘汰快照、flushBatch、持久化拷贝等依赖 Val 可拷贝/移动的代码不受影响。
*/
struct Val {
	std::string typeName;								// 类型名称
	long long dataSize;									// 数据大小
	bool isPermanent;									// 是否永不过期
	std::chrono::system_clock::time_point expireTime;	// 过期时间
	std::atomic<long long> updateTime{ 0 };				// 最近读命中/插入时刻（µs since epoch，relaxed）
	bool isDirty;										// 脏数据标记（true为脏，false为非脏）
	std::shared_ptr<std::any> entity;					// 值实体（指针：写入时实体移进堆，命中/回填共享同一实体，零拷贝）

	Val() = default;
	// 拷贝/移动：原子字段按 relaxed load 取值（无并发写的移动源语义足够；拷贝用于锁内快照）
	Val(const Val& o)
		: typeName(o.typeName), dataSize(o.dataSize), isPermanent(o.isPermanent),
		expireTime(o.expireTime),
		updateTime(o.updateTime.load(std::memory_order_relaxed)),
		isDirty(o.isDirty), entity(o.entity) {}
	Val& operator=(const Val& o) {
		if (this != &o) {
			typeName = o.typeName;
			dataSize = o.dataSize;
			isPermanent = o.isPermanent;
			expireTime = o.expireTime;
			updateTime.store(o.updateTime.load(std::memory_order_relaxed), std::memory_order_relaxed);
			isDirty = o.isDirty;
			entity = o.entity;
		}
		return *this;
	}
	Val(Val&& o) noexcept
		: typeName(std::move(o.typeName)), dataSize(o.dataSize), isPermanent(o.isPermanent),
		expireTime(o.expireTime),
		updateTime(o.updateTime.load(std::memory_order_relaxed)),
		isDirty(o.isDirty), entity(std::move(o.entity)) {}
	Val& operator=(Val&& o) noexcept {
		if (this != &o) {
			typeName = std::move(o.typeName);
			dataSize = o.dataSize;
			isPermanent = o.isPermanent;
			expireTime = o.expireTime;
			updateTime.store(o.updateTime.load(std::memory_order_relaxed), std::memory_order_relaxed);
			isDirty = o.isDirty;
			entity = std::move(o.entity);
		}
		return *this;
	}
};

// 查询结果：selData 的 future 返回体，一个 future 带回错误码与查询实体
struct SelResult {
	int resCode;		// 错误码（SUCCESS 成功 / FIND_FAILED 不存在 / EXPIRED 已过期）
	std::shared_ptr<std::any> entity;	// 查询到的实体（指针，失败时为空；只读约定，修改请走 modData）
};



#endif // !_DATA_TYPE_H_
