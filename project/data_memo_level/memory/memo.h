#pragma once
#ifndef _MEMO_H_
#define _MEMO_H_

#include <any>
#include <string>
#include <unordered_map>

/*
	基本数据类型别名
	缓存存储键值对：键为变量名(string)，值为以下基本类型之一
*/
typedef int INT;
typedef long long LONG;
typedef double DOUBLE;
typedef std::string STRING;

/*
	自定义类型值
	数据库只存数据不存方法，自定义类型（结构体/类）作为值实体整体存入
	type_name 标记其类型，供 CRUD 分发和后续持久化使用
*/
struct CustomValue {
	std::string type_name;   // 类型标记
	std::any entity;         // 值实体
};

/*
	内存存储（缓存层核心容器）
	每种基本类型一个 map，类型由 map 本身隐含，值实体直接存储
	自定义类型统一存入 custom_variable，值为 类型标记 + 值实体
	所有数据均为哈希存储（unordered_map）

	说明：成员暂为 public，等加入线程锁 / LRU / 过期策略时再封装
*/
class Memo {
public:
	std::unordered_map<std::string, INT>           int_variable;
	std::unordered_map<std::string, LONG>          long_variable;
	std::unordered_map<std::string, DOUBLE>        double_variable;
	std::unordered_map<std::string, STRING>        string_variable;
	std::unordered_map<std::string, CustomValue>   custom_variable;
};


#endif