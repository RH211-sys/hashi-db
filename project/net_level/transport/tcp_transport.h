#pragma once
#ifndef _MYDB_NET_TCP_TRANSPORT_H_
#define _MYDB_NET_TCP_TRANSPORT_H_

/*
    模块名：TCP 网络传输
    模块地位：Transport 抽象的普通 TCP 传输实现。
    模块功能描述：提供拥有 socket 的非阻塞 plain TCP 传输实现。
*/

#include "transport.h"
#include <cstdint>

namespace mydb::net {

/*
    类名：TcpTransport
    地位：Transport 抽象的普通 TCP 传输实现。
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
        传参：nativeHandle：已接受连接的原生 socket 句柄
        功能：接管 socket 并在 Linux 上设置为非阻塞模式
        返回值：无
    */
    explicit TcpTransport(std::intptr_t nativeHandle) noexcept;

    /*
        函数：~TcpTransport
        传参：无
        功能：关闭并释放当前拥有的 socket
        返回值：无
    */
    ~TcpTransport() override;

    /*
        函数：TcpTransport 复制构造
        传参：源对象：待复制的 TCP 传输对象
        功能：禁止复制并共享底层 socket 所有权
        返回值：无
    */
    TcpTransport(const TcpTransport&) = delete;
    /*
        函数：TcpTransport 复制赋值
        传参：源对象：待复制赋值的 TCP 传输对象
        功能：禁止共享或替换底层 socket 所有权
        返回值：赋值目标引用类型；该函数已删除，不可调用
    */
    TcpTransport& operator=(const TcpTransport&) = delete;
    /*
        函数：TcpTransport 移动构造
        传参：源对象：待移动的 TCP 传输对象
        功能：禁止转移由该对象管理的 socket 所有权
        返回值：无
    */
    TcpTransport(TcpTransport&&) = delete;
    /*
        函数：TcpTransport 移动赋值
        传参：源对象：待移动赋值的 TCP 传输对象
        功能：禁止替换该对象管理的 socket 所有权
        返回值：赋值目标引用类型；该函数已删除，不可调用
    */
    TcpTransport& operator=(TcpTransport&&) = delete;

    /*
        函数：nativeHandle
        传参：无
        功能：读取当前拥有的原生 socket 句柄
        返回值：有效句柄；初始化失败时返回 -1
    */
    std::intptr_t nativeHandle() const noexcept override;

    /*
        函数：handshake
        传参：无
        功能：完成 plain TCP 传输的握手阶段
        返回值：传输操作结果
    */
    TransportResult handshake() override;

    /*
        函数：read
        传参：buffer：接收目标；maxBytes：本次最多读取字节数
        功能：从非阻塞 socket 读取 TCP 字节流
        返回值：传输操作结果
    */
    TransportResult read(ByteBuffer& buffer, std::size_t maxBytes) override;

    /*
        函数：write
        传参：buffer：待发送字节视图；offset：已发送位置
        功能：向非阻塞 socket 写出 TCP 字节流
        返回值：传输操作结果
    */
    TransportResult write(std::span<const std::uint8_t> buffer, std::size_t& offset) override;

    /*
        函数：events
        传参：无
        功能：读取当前传输需要监听的 I/O 事件
        返回值：TransportEvent 位掩码
    */
    std::uint8_t events() const override;

    /*
        函数：close
        传参：无
        功能：关闭并释放当前拥有的 socket
        返回值：无
    */
    void close() override;
};

}

#endif
