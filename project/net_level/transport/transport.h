#pragma once
#ifndef _MYDB_NET_TRANSPORT_H_
#define _MYDB_NET_TRANSPORT_H_

/*
    模块名：网络传输抽象
    模块地位：网络层连接传输的统一接口，供连接模块屏蔽 TCP 与 TLS 差异。
    模块功能描述：统一 plain TCP 和 TLS 非阻塞传输的读写、握手、关闭及事件需求，屏蔽平台 socket 细节。
*/

#include "../common/net_types.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace mydb::net {

/*
    类型名：TransportResult
    功能：表示非阻塞传输操作的结果。
*/
enum class TransportResult : std::uint8_t {
    OK,                                           // 成功：本次读写或握手取得进展
    WOULD_BLOCK,                                  // 暂不可用：需要等待下一次 I/O 事件
    PEER_CLOSED,                                  // 对端关闭：收到 FIN 或 TLS close_notify
    FAILED                                        // 失败：不可恢复的 socket 或 TLS 错误
};

/*
    类型名：TransportEvent
    功能：表示传输层当前需要关注的 I/O 事件。
*/
enum TransportEvent : std::uint8_t {
    WANT_READ = 1U << 0,                          // 读事件：传输层等待可读数据
    WANT_WRITE = 1U << 1                          // 写事件：TLS 或输出等待可写
};

/*
    类名：Transport
    地位：网络传输层向 Connection 提供的统一传输边界。
    功能：定义 plain TCP 和 TLS 传输的统一非阻塞接口。
        - 屏蔽底层 socket 和 OpenSSL 细节。
        - 将 WANT_READ/WANT_WRITE 转换为 Reactor 可理解的事件。
        - 禁止在网络线程中阻塞等待。
    友元类：无
*/
class Transport {
public:
    /*
        函数：~Transport
        传参：无
        功能：销毁传输对象并释放其平台资源
        返回值：无
    */
    virtual ~Transport() = default;

    /*
        函数：nativeHandle
        传参：无
        功能：返回底层原生句柄的整数表示，不暴露平台 socket 类型
        返回值：句柄；不可用时返回 -1
    */
    virtual std::intptr_t nativeHandle() const noexcept = 0;

    /*
        函数：handshake
        传参：无
        功能：推进非阻塞传输握手；plain TCP 可直接完成
        返回值：TransportResult
    */
    virtual TransportResult handshake() = 0;

    /*
        函数：read
        传参：buffer：接收目标；maxBytes：本次最多读取的字节数
        功能：从传输层读取一段字节，不允许阻塞等待
        返回值：TransportResult
    */
    virtual TransportResult read(ByteBuffer& buffer, std::size_t maxBytes) = 0;

    /*
        函数：write
        传参：buffer：待发送字节视图；offset：已发送位置
        功能：向传输层写出一段字节，不复制缓冲内容且不允许阻塞等待
        返回值：TransportResult
    */
    virtual TransportResult write(std::span<const std::uint8_t> buffer, std::size_t& offset) = 0;

    /*
        函数：events
        传参：无
        功能：返回当前 transport 需要监听的读写事件
        返回值：TransportEvent 位掩码
    */
    virtual std::uint8_t events() const = 0;

    /*
        函数：close
        传参：无
        功能：关闭传输层，释放 socket 或 TLS 资源
        返回值：无
    */
    virtual void close() = 0;
};

}

#endif
