#pragma once

#include <any>
#include <string>
#include <unordered_map>

#include "../memo.h"

/*
	协议：类型索引与操作码
	类型索引：0=int 1=long 2=double 3=string 4=自定义
	操作码：INSERT=5 DELETE=6 MODIFY=7 SELECT=8
*/
#define INT_OPT 0
#define LONG_OPT 1
#define DOUBLE_OPT 2
#define STRING_OPT 3
#define CUSTOM_OPT 4

#define INSERT 5
#define DELETE 6
#define MODIFY 7
#define SELECT 8


/*
	缓存 CRUD 操作类（统一操作入口）
	通过引用持有 Memo，操作其中的各类型哈希表

	type 取值：基本类型为 int / long / double / string
	          自定义类型为该自定义类型的名字
	op 取值：INSERT / DELETE / MODIFY / SELECT（见上方协议宏）
	val：操作值（插入/修改传值，删除/查询传空 any）
	res：查询时用于返回结果（字符串形式），其余传 nullptr
	返回：成功 true / 失败 false
*/
class DataOpt {
public:
	explicit DataOpt(Memo& memo);

	// 统一操作入口：先查反射表（变量名存在 + 类型匹配），再分派到对应类型操作
	bool baseOperation(const std::string& type, const std::string& name, int op,
	                   std::any val, std::string* res);

private:
	// 类型名 -> 类型索引（INT_OPT / LONG_OPT / DOUBLE_OPT / STRING_OPT / CUSTOM_OPT）
	int typeToIndex(const std::string& type) const;

	// 各类型操作函数（支持 INSERT / DELETE / SELECT / MODIFY）
	bool intOp(const std::string& name, const std::string& type, int op, std::any val, std::string* res);
	bool longOp(const std::string& name, const std::string& type, int op, std::any val, std::string* res);
	bool doubleOp(const std::string& name, const std::string& type, int op, std::any val, std::string* res);
	bool stringOp(const std::string& name, const std::string& type, int op, std::any val, std::string* res);
	bool customOp(const std::string& name, const std::string& type, int op, std::any val, std::string* res);

	using OpFunc = bool (DataOpt::*)(const std::string&, const std::string&, int, std::any, std::string*);
	// 函数地址数组：索引 = 类型索引（INT_OPT / LONG_OPT / DOUBLE_OPT / STRING_OPT / CUSTOM_OPT）
	OpFunc opFuncs_[5] = {
		&DataOpt::intOp, &DataOpt::longOp, &DataOpt::doubleOp,
		&DataOpt::stringOp, &DataOpt::customOp
	};

	Memo& memo_;
	// 反射表：变量名 -> 类型索引
	// 一次查表即可同时得到"变量名是否存在"和"类型是否匹配"
	std::unordered_map<std::string, int> reflect_;
};
