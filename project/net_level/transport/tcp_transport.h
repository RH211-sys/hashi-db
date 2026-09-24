#pragma once
#ifndef _MYDB_NET_TCP_TRANSPORT_H_
#define _MYDB_NET_TCP_TRANSPORT_H_

/*
    模块名：TCP 网络传输
    功能描述：提供拥有 socket 的非阻塞 plain TCP 传输实现。
*/

#include "transport.h"
#include <cstdint>

namespace mydb::net {

/*
    类名：TcpTransport
    功能：以非阻塞方式收发 TCP 字节流并管理其 socket 生命周期。
        - 构造时接管 native handle，并在 Linux 上设置 O_NONBLOCK。
        - 不执行 TLS 握手；plain TCP 握手立即完成。
        - 不在共享接口中暴露平台 socket 类型。
*/
class TcpTransport final : public Transport {
private:
    std::intptr_t socketHandle = -1;               // socket：被当前传输对象拥有的原生句柄
    std::uint8_t desiredEvents = WANT_READ;        // 关注事件：本次非阻塞操作所需的就绪类型
    bool available = false;                        // 可用状态：Linux socket 初始化是否成功

public:
    /*
        函数：TcpTransport
        参数：nativeHandle：已接受连接的原生 socket 句柄
        功能：接管 socket 并在 Linux 上设置为非阻塞模式
        返回：无
    */
    explicit TcpTransport(std::intptr_t nativeHandle) noexcept;

    /*
        函数：~TcpTransport
        参数：无
        功能：关闭并释放当前拥有的 socket
        返回：无
    */
    ~TcpTransport() override;

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;
    TcpTransport(TcpTransport&&) = delete;
    TcpTransport& operator=(TcpTransport&&) = delete;

    std::intptr_t nativeHandle() const noexcept override;
    TransportResult handshake() override;
    TransportResult read(ByteBuffer& buffer, std::size_t maxBytes) override;
    TransportResult write(const ByteBuffer& buffer, std::size_t& offset) override;
    std::uint8_t events() const override;
    void close() override;
};

}

#endif
