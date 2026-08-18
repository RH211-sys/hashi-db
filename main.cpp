#include <iostream>

#include "data_memo_level/memory/data_opt/data_opt.h"

int main() {
	Memo memo;
	DataOpt opt(memo);

	// ===== 插入（INSERT）=====
	opt.baseOperation("int", "age", INSERT, std::any(INT(25)), nullptr);
	opt.baseOperation("long", "score", INSERT, std::any(LONG(10000000000LL)), nullptr);
	opt.baseOperation("double", "pi", INSERT, std::any(DOUBLE(3.14)), nullptr);
	opt.baseOperation("string", "name", INSERT, std::any(STRING("tom")), nullptr);

	// 自定义类型：结构体整体存入
	struct User {
		INT id;
		STRING name;
	};
	opt.baseOperation("User", "user1", INSERT, std::any(User{1, "alice"}), nullptr);

	// ===== 查询（SELECT）：结果写入 string* =====
	std::string res;
	opt.baseOperation("int", "age", SELECT, {}, &res);
	std::cout << "age   = " << res << std::endl;
	opt.baseOperation("double", "pi", SELECT, {}, &res);
	std::cout << "pi    = " << res << std::endl;
	opt.baseOperation("string", "name", SELECT, {}, &res);
	std::cout << "name  = " << res << std::endl;

	// ===== 失败场景 =====
	// 类型不匹配：age 已是 int，不能再以 string 插入
	std::cout << "insert age as string : " << opt.baseOperation("string", "age", INSERT, std::any(STRING("x")), nullptr) << std::endl;
	// 变量不存在
	std::cout << "find not_exist       : " << opt.baseOperation("int", "not_exist", SELECT, {}, &res) << std::endl;
	// 修改不存在的变量
	std::cout << "modify not_exist     : " << opt.baseOperation("int", "not_exist", MODIFY, std::any(INT(1)), nullptr) << std::endl;

	// ===== 修改（MODIFY）=====
	std::cout << "modify age           : " << opt.baseOperation("int", "age", MODIFY, std::any(INT(30)), nullptr) << std::endl;
	opt.baseOperation("int", "age", SELECT, {}, &res);
	std::cout << "age after modify = " << res << std::endl;

	// ===== 删除（DELETE）=====
	std::cout << "delete name          : " << opt.baseOperation("string", "name", DELETE, {}, nullptr) << std::endl;
	std::cout << "delete name again    : " << opt.baseOperation("string", "name", DELETE, {}, nullptr) << std::endl;
	// 删除后反射记录同步清理，再次查询返回 false
	std::cout << "find deleted name    : " << opt.baseOperation("string", "name", SELECT, {}, &res) << std::endl;

	return 0;
}
