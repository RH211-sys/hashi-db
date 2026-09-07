#pragma once
#ifndef _PROTOCAL_H_
#define _PROTOCAL_H_


/* ========== 错误码 ========== */
const int SUCCESS = 200;		// 操作成功
const int KEY_EXIST = 201;		// 变量已存在
const int FIND_FAILED = 202;	// 未找到该变量
const int TYPE_VALID = 203;		// 未注册该类型
const int EXPIRED = 204;		// 数据已过期
const int MEMO_OUT = 300;		// 容量超限（内存满；磁盘写路径压缩后仍超过 maxSize 时也返回此码）
const int LONG_NAME = 301;		// 名称过长
const int UNKNOWN_ERROR = 400;	// 未知错误
const int FILE_OPEN_FILED = 500; // 文件打开失败



/* ========== 磁盘协议 ========== */
const char CHECK_VALID   = static_cast<char>(0x5A);	// 活记录（数据完整）
const char CHECK_DELETED = static_cast<char>(0xFF);	// 已删/空洞段：删除即空洞（1 字节删除标记）；顺序扫描按 dataSize 跳过、段可复用
									//   见 doc/arch/data_memo_doc/实现阶段/二轮优化/空洞删除段设计.md
const char CHECK_BROKEN  = static_cast<char>(0x00);	// 残尾（写一半崩溃）：重建时截断到此处
const int NAME_LEN = 32;			// 变量名定长上限（写不满补 '\0'，超过返回失败）
const int TYPE_LEN = 32;			// 类型名定长上限（写不满补 '\0'）
const int CODE_LEN = 1;				// 校验码长度
const int ENTITY_SIZE_LEN = 4;		// 实体数据大小长度
const int TIME_INFO_LEN = 17;		// 时间信息总长度
const int UPDATE_TIME_LEN = 8;		// 更新时间长度
const int EXPIRE_TIME_LEN = 8;		// 过期时间长度
const int IS_PERMANENT_LEN = 1;		// 是否永久长度



#endif // !_PROTOCAL_H_
