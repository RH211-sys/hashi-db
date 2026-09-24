#pragma once
#ifndef _MYDB_NET_TYPES_H_
#define _MYDB_NET_TYPES_H_

/*
    模块名：网络层公共类型
    功能描述：定义网络层各模块共享的连接标识、端点、字节缓冲和状态类型，避免公共接口依赖平台 socket 类型。
*/

#include <cstdint>
#include <string>
#include <vector>

namespace mydb::net {

using ConnectionId = std::uint64_t;             // 连接 ID：进程内唯一标识一个客户端连接，不能用 fd 代替
using ReactorId = std::uint32_t;                // Reactor ID：标识连接所属的事件循环线程
using RequestId = std::uint64_t;                // 请求 ID：客户端请求与服务端响应的关联标识
using ByteBuffer = std::vector<std::uint8_t>;   // 字节缓冲：保存网络输入、输出以及二进制字段

/*
    类型名：Endpoint
    功能：描述网络端点地址和端口，不直接依赖 sockaddr 等平台类型。
*/
struct Endpoint {
    std::string address;                         // 地址：IPv4、IPv6 或后续本地端点的文本表示
    std::uint16_t port = 0;                      // 端口：TCP 监听或对端端口
};

/*
    类型名：ServerState
    功能：描述网络服务端的生命周期状态。
*/
enum class ServerState : std::uint8_t {
    CREATED,                                     // 已创建：配置已保存，服务尚未启动
    RUNNING,                                     // 运行中：正在接受连接和处理请求
    DRAINING,                                    // 排空中：停止接收新请求，等待已提交请求完成
    STOPPED                                      // 已停止：监听器、Reactor 和连接均已关闭
};

/*
    类型名：ConnectionState
    功能：描述单个客户端连接的协议和关闭状态。
*/
enum class ConnectionState : std::uint8_t {
    ACCEPTED,                                    // 已接受：TCP 连接刚建立
    TLS_HANDSHAKE,                               // TLS 握手：正在推进非阻塞 TLS 握手
    WAIT_HELLO,                                  // 等待 HELLO：尚未完成协议版本协商
    WAIT_AUTH,                                   // 等待认证：协议已协商但会话尚未认证
    READY,                                       // 就绪：允许执行已授权的业务命令
    CLOSING,                                     // 关闭中：停止接收新请求并尝试发送剩余响应
    CLOSED                                       // 已关闭：连接资源已释放
};

/*
    类型名：RequestSource
    功能：标识请求来自远程客户端还是本地管理入口。
*/
enum class RequestSource : std::uint8_t {
    REMOTE = 0,                                  // 远程请求：经过 TCP/TLS 连接进入
    LOCAL_ADMIN = 1                              // 本地管理请求：进程内直接进入执行层
};

/*
    类型名：ErrorCode
    功能：定义网络层对外稳定返回的错误分类，不直接复用存储层内部错误码。
*/
enum class ErrorCode : std::uint16_t {
    OK = 0,                                      // 成功：请求已正常处理
    BAD_FRAME = 100,                             // 坏帧：固定头、长度或 TLV 格式非法
    UNSUPPORTED = 101,                           // 不支持：版本、特性或字段暂不支持
    NOAUTH = 102,                                // 未认证：当前会话尚未通过认证
    UNKNOWN_COMMAND = 400,                       // 未知命令：opcode 不在协议定义中
    FORBIDDEN = 401,                             // 无权限：会话不允许执行当前命令
    TYPE_NOT_REGISTERED = 402,                   // 类型未注册：wire type 没有显式 codec
    BUSY = 300,                                  // 忙：服务容量或在途请求达到上限
    TOO_LARGE = 301,                             // 过大：帧、字段或输出超过限制
    INTERNAL = 500,                              // 内部错误：网络层未分类的服务错误
    STORAGE_UNAVAILABLE = 501,                  // 存储不可用：存储执行层无法接受请求
    TLS_ERROR = 502                              // TLS 错误：握手或加密传输失败
};

}

#endif
