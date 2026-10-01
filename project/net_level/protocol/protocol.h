#pragma once
#ifndef _MYDB_NET_PROTOCOL_H_
#define _MYDB_NET_PROTOCOL_H_

/*
    模块名：网络协议基础类型
    模块地位：帧编解码、命令解析与执行器共享的协议数据类型边界。
    模块功能描述：定义固定帧头、命令码、TLV 字段和请求/响应对象；不负责字节流读取和业务执行。
*/

#include "../common/net_types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace mydb::net {

constexpr std::uint16_t PROTOCOL_VERSION = 1;                         // 当前协议版本：固定头和字段语义的版本号
constexpr std::uint16_t BASE_HEADER_LENGTH = 24;                      // 基础头长度：固定头在线上的字节数
constexpr std::uint32_t DEFAULT_MAX_FRAME_BYTES = 16U * 1024U * 1024U; // 默认帧上限：限制单帧内存分，16MB

/*
    类型名：Opcode
    功能：定义客户端请求和服务端命令的稳定编号。
*/
enum class Opcode : std::uint16_t {
    HELLO = 0x0001,                              // 握手：协商协议版本和特性
    AUTH = 0x0002,                               // 认证：验证客户端身份
    PING = 0x0003,                               // 探活：返回客户端携带的可选数据
    QUIT = 0x0004,                               // 退出：发送响应后关闭连接
    GET = 0x0010,                                // 查询：读取指定 key 的值
    ADD = 0x0011,                                // 新增：创建一个不存在的 key
    UPDATE = 0x0012,                             // 修改：替换一个已存在的 key
    DELETE_DATA = 0x0013,                        // 删除：删除指定 key
    PERSIST = 0x0020,                            // 持久化：将指定数据或全部脏数据刷盘
    FLUSH = 0x0021,                              // 刷盘：执行过期清理和持久化
    REWRITE = 0x0022,                            // 重写：整理磁盘文件和空洞
    STATS = 0x0030,                              // 统计：读取服务或存储统计
    CLIENT_LIST = 0x0031,                        // 客户端列表：读取连接快照
    CLIENT_KILL = 0x0032                         // 客户端踢出：按 ConnectionId 关闭连接
};

/*
    类型名：FrameFlag
    功能：定义帧头 flags 字段中的位标记。
*/
enum FrameFlag : std::uint8_t {
    RESPONSE = 1U << 0,                          // 响应标记：当前帧是服务端响应
    ERROR = 1U << 1,                             // 错误标记：响应包含错误状态
    COMPRESSED = 1U << 2,                        // 压缩标记：首版保留，未协商时必须拒绝
    MORE = 1U << 3                               // 分片标记：首版保留，表示后续仍有帧
};

/*
    类型名：FieldType
    功能：定义 TLV 字段的值类型，控制字段解析方式。
*/
enum class FieldType : std::uint8_t {
    BYTES = 1,                                   // 二进制：不进行文本编码校验
    UTF8 = 2,                                    // 文本：按 UTF-8 约定解释
    U64 = 3,                                     // 无符号整数：网络序整数
    I64 = 4,                                     // 有符号整数：网络序整数
    BOOL = 5,                                    // 布尔值：协议约定的单字节值
    NESTED = 6                                   // 嵌套字段：内部继续使用 TLV
};

/*
    类型名：FrameHeader
    地位：网络帧编解码与协议处理之间的固定头部表示。
    功能：表示 24 字节基础帧头在主机内存中的结构化形式。
*/
struct FrameHeader {
    std::uint8_t version = 0;                    // 版本：帧使用的协议版本
    std::uint8_t flags = 0;                      // 标志：FrameFlag 位掩码
    Opcode opcode = Opcode::PING;                // 命令码：当前帧对应的操作
    std::uint16_t headerLength = BASE_HEADER_LENGTH; // 头长度：基础头和扩展头总长度
    std::uint32_t bodyLength = 0;                // Body 长度：TLV 内容字节数
    RequestId requestId = 0;                     // 请求 ID：关联请求和响应
};

/*
    类型名：TlvField
    地位：Frame 与 CommandParser 之间的协议字段表示。
    功能：表示一个 TLV 编码字段。
*/
struct TlvField {
    std::uint16_t fieldId = 0;                   // 字段 ID：由具体 opcode 定义语义
    FieldType type = FieldType::BYTES;           // 字段类型：决定 value 的解释方式
    ByteBuffer value;                            // 字段值：不包含 TLV 头部
};

/*
    类型名：Frame
    地位：帧编解码器与命令解析器之间的完整协议帧表示。
    功能：保存一个已完成解析或待编码的协议帧。
*/
struct Frame {
    FrameHeader header;                          // 帧头：协议控制信息
    std::vector<TlvField> fields;                // 字段列表：帧 Body 中的 TLV 字段
};

/*
    类型名：CommandRequest
    地位：网络层提交给执行层的结构化请求契约。
    功能：保存命令解析后的结构化请求，作为网络层到执行层的输入。
*/
struct CommandRequest {
    ConnectionId connectionId = 0;              // 连接 ID：请求所属连接
    RequestId requestId = 0;                    // 请求 ID：响应必须原样带回
    RequestSource source = RequestSource::REMOTE; // 来源：远程客户端或本地管理入口
    std::string principal;                       // 主体：认证后的用户或管理身份
    Endpoint peer;                               // 对端：远程请求的来源地址
    Opcode opcode = Opcode::PING;                // 命令码：待执行操作
    std::vector<TlvField> fields;                // 参数：经解析和基础校验的字段
};

/*
    类型名：CommandResponse
    地位：执行层返回给网络层的结构化结果契约。
    功能：保存执行层返回给网络连接的结构化响应。
*/
struct CommandResponse {
    ConnectionId connectionId = 0;              // 连接 ID：响应所属连接
    RequestId requestId = 0;                    // 请求 ID：与原请求一一对应
    ErrorCode status = ErrorCode::OK;            // 状态码：网络层稳定错误分类
    std::string message;                         // 错误信息：有界、可脱敏的辅助说明
    std::vector<TlvField> fields;                // 结果字段：成功结果或错误附加信息
    bool closeAfterWrite = false;               // 关闭标记：写完当前响应后是否关闭连接
};

}

#endif
