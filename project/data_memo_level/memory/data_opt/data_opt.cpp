#include <sstream>
#include <utility>

#include "data_opt.h"

DataOpt::DataOpt(Memo& memo) : memo_(memo) {}

int DataOpt::typeToIndex(const std::string& type) const {
	if (type == "int")    return INT_OPT;
	if (type == "long")   return LONG_OPT;
	if (type == "double") return DOUBLE_OPT;
	if (type == "string") return STRING_OPT;
	return CUSTOM_OPT;   // 其余视为自定义类型
}

bool DataOpt::baseOperation(const std::string& type, const std::string& name, int op,
                            std::any val, std::string* res) {
	const int idx = typeToIndex(type);

	// 一次查反射表：变量名是否存在 + 类型是否匹配
	// 自定义类型额外要求类型名一致（不同自定义类型索引都是 CUSTOM_OPT）
	bool typeMatched = false;
	auto it = reflect_.find(name);
	if (it != reflect_.end() && it->second == idx) {
		if (idx == CUSTOM_OPT) {
			auto cit = memo_.custom_variable.find(name);
			typeMatched = cit != memo_.custom_variable.end() && cit->second.type_name == type;
		} else {
			typeMatched = true;
		}
	}

	if (op == INSERT) {
		// 插入：变量名已存在则类型必须匹配，不存在则新增反射记录
		if (it != reflect_.end() && !typeMatched) return false;
		reflect_[name] = idx;
		return (this->*opFuncs_[idx])(name, type, op, std::move(val), res);
	}

	// 删除/查询/修改：变量名存在且类型匹配才继续
	if (!typeMatched) return false;
	const bool ok = (this->*opFuncs_[idx])(name, type, op, std::move(val), res);
	if (op == DELETE && ok) reflect_.erase(name);   // 删除成功后单独清理反射记录，否则不应该清理，因为删除失败
	return ok;
}

// ===== 各类型操作函数：INSERT / DELETE / SELECT / MODIFY =====

bool DataOpt::intOp(const std::string& name, const std::string&, int op, std::any val, std::string* res) {
	switch (op) {
	case INSERT: memo_.int_variable[name] = std::any_cast<INT>(val); return true;
	case DELETE: memo_.int_variable.erase(name);                     return true;
	case SELECT: if (res) *res = std::to_string(memo_.int_variable.at(name)); return true;
	case MODIFY: memo_.int_variable[name] = std::any_cast<INT>(val); return true;
	}
	return false;
}

bool DataOpt::longOp(const std::string& name, const std::string&, int op, std::any val, std::string* res) {
	switch (op) {
	case INSERT: memo_.long_variable[name] = std::any_cast<LONG>(val); return true;
	case DELETE: memo_.long_variable.erase(name);                      return true;
	case SELECT: if (res) *res = std::to_string(memo_.long_variable.at(name)); return true;
	case MODIFY: memo_.long_variable[name] = std::any_cast<LONG>(val); return true;
	}
	return false;
}

bool DataOpt::doubleOp(const std::string& name, const std::string&, int op, std::any val, std::string* res) {
	switch (op) {
	case INSERT: memo_.double_variable[name] = std::any_cast<DOUBLE>(val); return true;
	case DELETE: memo_.double_variable.erase(name);                        return true;
	case SELECT:
		// 用 ostringstream 而非 to_string：避免 double 被打印成 3.140000
		if (res) {
			std::ostringstream oss;
			oss << memo_.double_variable.at(name);
			*res = oss.str();
		}
		return true;
	case MODIFY: memo_.double_variable[name] = std::any_cast<DOUBLE>(val); return true;
	}
	return false;
}

bool DataOpt::stringOp(const std::string& name, const std::string&, int op, std::any val, std::string* res) {
	switch (op) {
	case INSERT: memo_.string_variable[name] = std::any_cast<STRING>(val); return true;
	case DELETE: memo_.string_variable.erase(name);                        return true;
	case SELECT: if (res) *res = memo_.string_variable.at(name);           return true;
	case MODIFY: memo_.string_variable[name] = std::any_cast<STRING>(val); return true;
	}
	return false;
}

bool DataOpt::customOp(const std::string& name, const std::string& type, int op, std::any val, std::string* res) {
	switch (op) {
	case INSERT: memo_.custom_variable[name] = CustomValue{type, std::move(val)}; return true;
	case DELETE: memo_.custom_variable.erase(name);                               return true;
	case SELECT:
		// TODO: 自定义类型序列化待实现（协议层/磁盘层需要），暂时返回类型名
		if (res) *res = memo_.custom_variable.at(name).type_name;
		return true;
	case MODIFY: memo_.custom_variable[name] = CustomValue{type, std::move(val)}; return true;
	}
	return false;
}
