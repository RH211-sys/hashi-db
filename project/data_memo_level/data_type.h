#pragma once
#ifndef _DATA_TYPE_H_
#define _DATA_TYPE_H_


/*
	该文件通过#include用户自定义的数据类型，只需include
	用户需要专门创建一个文件A，定义自定义类，然后在该文件中include文件A
	自定义类，必要的方法：
	1. 获取数据类型的类名(getClassName)之类的函数
*/
#include <unordered_set>
#include <string>
#include <any>
#include <chrono>
#include "protocol.h"
#include <memory>

inline std::unordered_set<std::string> totalType;
/*
	注册数据类型的方法
	参数：数据类型名称
	功能：向totalType中添加该类型
*/
#define DEFINE_DATA_TYPE(name) totalType.insert(#name);

struct Val {
	std::string typeName;								// 类型名称
	std::chrono::system_clock::time_point expireTime;	// 过期时间
	std::chrono::system_clock::time_point createTime;	// 创建时间
	bool isDirty;										// 脏数据标记（true为脏，false为非脏）
	std::any entity;									// 值实体
};

#endif // !_DATA_TYPE_H_
