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
// 序列化函数：把 any 里的对象打成字节（写盘用）
using SerializeFunc = std::vector<char>(*)(const std::any& obj);
// 反序列化函数：把字节还原成对象，包进 any（读盘用）
using DeserializeFunc = std::any(*)(const std::vector<char>& bytes);

// 类型注册表：typeName → {序列化函数, 反序列化函数}，程序启动注册，运行期只读
inline std::unordered_map<std::string, std::pair<SerializeFunc, DeserializeFunc>> typeReg;

// 序列化：cereal 把对象写入内存流，取出字节
template <typename T>
std::vector<char> toBytes(const std::any& obj) {
    std::stringstream ss;
    {
        cereal::BinaryOutputArchive ar(ss);
        ar(std::any_cast<const T&>(obj));
    }
    std::string s = ss.str();
    return std::vector<char>(s.begin(), s.end());
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

struct Val {
	std::string typeName;								// 类型名称
	long long dataSize;									// 数据大小
	bool isPermanent;									// 是否永不过期
	std::chrono::system_clock::time_point expireTime;	// 过期时间
	std::chrono::system_clock::time_point updateTime;	// 更新时间
	bool isDirty;										// 脏数据标记（true为脏，false为非脏）
	std::any entity;									// 值实体
};

// 查询结果：selData 的 future 返回体，一个 future 带回错误码与查询数据
struct SelResult {
	int resCode;		// 错误码（SUCCESS 成功 / FIND_FAILED 不存在 / EXPIRED 已过期）
	std::any data;		// 查询到的实体（失败时为空）
};



#endif // !_DATA_TYPE_H_
