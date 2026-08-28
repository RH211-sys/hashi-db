#pragma once
#ifndef _PROTOCAL_H_
#define _PROTOCAL_H_


/* ========== 错误码 ========== */
const int SUCCESS = 200;		// 操作成功
const int KEY_EXIST = 201;		// 变量已存在
const int FIND_FAILED = 202;	// 未找到该变量
const int TYPE_VALID = 203;		// 未注册该类型
const int MEMO_OUT = 300;		// 内存爆满
const int LONG_VAR_NAME = 301;	// 变量名过长
const int UNKNOWN_ERROR = 400;	// 未知错误
const int FILE_OPEN_FILED = 500; // 文件打开失败



/* ========== 磁盘协议 ========== */
const char CHECK_VALID = 0x5A;		// 数据完整（重建时读入）
const char CHECK_BROKEN = 0x00;		// 数据不完整（残尾，重建时截断）
const int NAME_LEN = 64;			// 变量名定长上限（写不满补 '\0'，超过返回失败）

#endif // !_PROTOCAL_H_
