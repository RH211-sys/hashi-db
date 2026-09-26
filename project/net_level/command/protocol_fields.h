#pragma once
#ifndef _MYDB_NET_PROTOCOL_FIELDS_H_
#define _MYDB_NET_PROTOCOL_FIELDS_H_

/*
    模块名：协议字段编号
    模块地位：维护网络协议字段编号这一稳定 wire contract。
    模块功能描述：定义协议 v1 TLV field_id。数值属于 wire contract，必须与协议设计文档同步维护。
*/

#include <cstdint>

namespace mydb::net {

enum class FieldId : std::uint16_t {
    PROTOCOL_VERSION = 1, // 协议版本：HELLO 协商的版本号
    FEATURE_BITS = 2,     // 特性位：HELLO 请求声明的能力集合
    CLIENT_NAME = 3,      // 客户端名称：HELLO 请求提供的客户端标识文本
    USERNAME = 4,         // 用户名：AUTH 请求的身份名称
    PASSWORD = 5,         // 口令：AUTH 请求的凭据字节
    PAYLOAD = 6,          // 负载：PING 请求或响应的可选数据
    KEY = 7,              // 键：存储操作使用的键字节
    TYPE_NAME = 8,        // 类型名：存储值对应的注册类型
    VALUE = 9,            // 值：存储操作携带的数据
    TTL = 10,             // 生存时间：存储项的有效期
    SCOPE = 11,            // 范围：管理查询的可选筛选条件
    CONNECTION_ID = 12,   // 连接标识：管理命令指定的目标连接
    STATUS_CODE = 13,     // 状态码：响应携带的结果类别
    MESSAGE = 14          // 消息：响应携带的可读说明文本
};

/*
    函数：fieldId
    传参：id：协议字段枚举值
    功能：获取字段在 wire contract 中使用的数值编号
    返回值：字段编号
*/
constexpr std::uint16_t fieldId(FieldId id) noexcept {
    return static_cast<std::uint16_t>(id);
}

}

#endif
