#pragma once
#ifndef _MYDB_NET_TLS_TRANSPORT_H_
#define _MYDB_NET_TLS_TRANSPORT_H_

/*
    模块名：TLS 网络传输
    模块地位：网络层 Transport 抽象的加密实现
    模块功能描述：以非阻塞方式推进 TLS 握手和加密读写，并隐藏 OpenSSL 类型。
*/

#include "transport.h"
#include <memory>
#include <string>

namespace mydb::net {

/*
    地位：为所有 TLS 连接共享服务端安全配置
    功能：加载证书与私钥并持有不可变的 OpenSSL 服务端上下文。
*/
class TlsServerContext final {
private:
    struct Impl;
    std::unique_ptr<Impl> impl;                   // 内部实现：封装 OpenSSL 上下文

    /*
        功能：创建已完成上下文初始化的服务端 TLS 对象
        传参：impl：已初始化的内部上下文
        返回值：无
    */
    explicit TlsServerContext(std::unique_ptr<Impl> impl) noexcept;
    friend class TlsTransport;

public:
    /*
        功能：创建并校验 TLS 服务端上下文
        传参：certificateFile：证书链文件；privateKeyFile：PEM 私钥文件
        返回值：有效上下文；配置或 OpenSSL 初始化失败时返回空指针
    */
    static std::shared_ptr<TlsServerContext> create(const std::string& certificateFile,
                                                     const std::string& privateKeyFile);

    /*
        功能：释放 TLS 服务端上下文
        传参：无
        返回值：无
    */
    ~TlsServerContext();

    TlsServerContext(const TlsServerContext&) = delete;
    TlsServerContext& operator=(const TlsServerContext&) = delete;
};

/*
    地位：已接受客户端连接的加密传输实现
    功能：在非阻塞 socket 上执行 TLS 握手、读写和资源关闭。
*/
class TlsTransport final : public Transport {
private:
    struct Impl;
    std::unique_ptr<Impl> impl;                   // 内部实现：管理 SSL 会话及底层 socket

public:
    /*
        功能：创建 TLS 连接传输并接管底层 socket 传输
        传参：socketTransport：拥有已接受 socket 的传输；context：共享 TLS 服务端上下文
        返回值：无
    */
    TlsTransport(std::unique_ptr<Transport> socketTransport,
                 std::shared_ptr<TlsServerContext> context);

    /*
        功能：关闭 TLS 会话与底层 socket
        传参：无
        返回值：无
    */
    ~TlsTransport() override;

    TlsTransport(const TlsTransport&) = delete;
    TlsTransport& operator=(const TlsTransport&) = delete;
    TlsTransport(TlsTransport&&) = delete;
    TlsTransport& operator=(TlsTransport&&) = delete;

    /*
        功能：返回底层非阻塞 socket 句柄
        传参：无
        返回值：原生句柄；TLS 初始化失败时返回 -1
    */
    std::intptr_t nativeHandle() const noexcept override;

    /*
        功能：推进服务端 TLS 握手
        传参：无
        返回值：传输操作结果
    */
    TransportResult handshake() override;

    /*
        功能：读取并解密一段 TLS 明文
        传参：buffer：接收目标；maxBytes：本次最多读取的字节数
        返回值：传输操作结果
    */
    TransportResult read(ByteBuffer& buffer, std::size_t maxBytes) override;

    /*
        功能：加密并写出一段 TLS 明文
        传参：buffer：待发送字节视图；offset：已发送位置
        返回值：传输操作结果
    */
    TransportResult write(std::span<const std::uint8_t> buffer, std::size_t& offset) override;

    /*
        功能：返回 TLS 当前等待的 socket 就绪方向
        传参：无
        返回值：TransportEvent 位掩码
    */
    std::uint8_t events() const override;

    /*
        功能：释放 TLS 会话并关闭底层 socket
        传参：无
        返回值：无
    */
    void close() override;
};

}

#endif
