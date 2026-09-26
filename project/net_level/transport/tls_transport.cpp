/*
    模块名：TLS 网络传输
    模块地位：网络层 Transport 抽象的加密实现
    模块功能描述：以非阻塞方式推进 TLS 握手和加密读写，并隐藏 OpenSSL 类型。
*/

#include "tls_transport.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace mydb::net {

/*
    地位：TlsServerContext 持有的 OpenSSL 原生上下文资源。
    功能：封装 SSL_CTX 生命周期并在销毁时释放对应资源。
*/
struct TlsServerContext::Impl {
    SSL_CTX* nativeContext = nullptr;              // OpenSSL 上下文：由共享服务端配置创建

    /*
        功能：释放 OpenSSL 服务端上下文
        传参：无
        返回值：无
    */
    ~Impl() {
        if (nativeContext != nullptr) {
            // OpenSSL 上下文已创建，释放其原生资源。
            SSL_CTX_free(nativeContext);
        }
    }
};

/*
    地位：TlsTransport 持有的单连接 TLS 会话状态。
    功能：管理 SSL 会话、底层 socket 传输和非阻塞重试数据。
*/
struct TlsTransport::Impl {
    std::unique_ptr<Transport> socketTransport;    // 底层 socket：负责句柄所有权和最终关闭
    std::shared_ptr<TlsServerContext> context;     // 服务上下文：保证 SSL_CTX 在会话期间存活
    SSL* session = nullptr;                        // TLS 会话：只由所属 Reactor 线程访问
    std::uint8_t desiredEvents = WANT_READ;         // 等待事件：OpenSSL 最近一次要求的就绪方向
    ByteBuffer readRetryBuffer;                     // 读取暂存区：在 SSL_read 重试期间保持地址稳定
    std::size_t readRetryBytes = 0;                 // 读取长度：重试时保持原始长度
    bool readPending = false;                       // 读取状态：标记未完成的 SSL_read 操作
};

namespace {

constexpr std::size_t TLS_READ_CHUNK_BYTES = 16 * 1024; // 读取块上限：限制单次 TLS 明文读取缓冲

/*
    功能：禁止 OpenSSL 通过默认交互界面请求私钥密码
    传参：无
    返回值：不提供密码
*/
int rejectPrivateKeyPassword(char*, int, int, void*) {
    return 0;
}

/*
    功能：把 OpenSSL 的非阻塞错误状态转换为传输结果
    传参：session：TLS 会话；result：OpenSSL 操作结果；desiredEvents：等待方向输出
    返回值：传输操作结果
*/
TransportResult translateSslFailure(SSL* session, int result, std::uint8_t& desiredEvents) {
    const int sslError = SSL_get_error(session, result); // OpenSSL 错误：映射当前操作的非阻塞状态
    if (sslError == SSL_ERROR_WANT_READ) {
        // OpenSSL 等待底层 socket 可读，记录读事件并请求稍后重试。
        desiredEvents = WANT_READ;
        return TransportResult::WOULD_BLOCK;
    }
    if (sslError == SSL_ERROR_WANT_WRITE) {
        // OpenSSL 等待底层 socket 可写，记录写事件并请求稍后重试。
        desiredEvents = WANT_WRITE;
        return TransportResult::WOULD_BLOCK;
    }
    if (sslError == SSL_ERROR_ZERO_RETURN) {
        // 对端通过 TLS close-notify 正常结束会话，报告对端关闭。
        desiredEvents = WANT_READ;
        return TransportResult::PEER_CLOSED;
    }
    desiredEvents = WANT_READ;
    return TransportResult::FAILED;
}

}

/*
    功能：保存已创建的 TLS 服务端上下文实现
    传参：impl：拥有 OpenSSL 上下文的实现对象
    返回值：无
*/
TlsServerContext::TlsServerContext(std::unique_ptr<Impl> impl) noexcept : impl(std::move(impl)) {}

/*
    功能：创建 TLS 服务端上下文并加载证书和私钥
    传参：certificateFile：证书链文件路径；privateKeyFile：私钥文件路径
    返回值：创建成功的共享上下文；任一配置步骤失败时返回空指针
*/
std::shared_ptr<TlsServerContext> TlsServerContext::create(const std::string& certificateFile,
                                                           const std::string& privateKeyFile) {
    ERR_clear_error();
    auto contextImpl = std::make_unique<Impl>(); // 上下文实现：暂存并初始化共享 TLS 服务端资源
    contextImpl->nativeContext = SSL_CTX_new(TLS_server_method());
    if (contextImpl->nativeContext == nullptr) {
        // OpenSSL 服务端上下文创建失败，清理错误队列并返回空结果。
        ERR_clear_error();
        return {};
    }
    SSL_CTX_set_default_passwd_cb(contextImpl->nativeContext, rejectPrivateKeyPassword);
    if (SSL_CTX_set_min_proto_version(contextImpl->nativeContext, TLS1_2_VERSION) != 1 ||
        SSL_CTX_use_certificate_chain_file(contextImpl->nativeContext, certificateFile.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(contextImpl->nativeContext, privateKeyFile.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(contextImpl->nativeContext) != 1) {
        // TLS 版本、证书链或私钥配置校验失败，清理错误队列并返回空结果。
        ERR_clear_error();
        return {};
    }
    return std::shared_ptr<TlsServerContext>(new TlsServerContext(std::move(contextImpl)));
}

/*
    功能：销毁 TLS 服务端上下文
    传参：无
    返回值：无
*/
TlsServerContext::~TlsServerContext() = default;

/*
    功能：创建 TLS 连接并绑定底层传输与共享服务端上下文
    传参：socketTransport：拥有底层 socket 的传输；context：TLS 服务端上下文
    返回值：无
*/
TlsTransport::TlsTransport(std::unique_ptr<Transport> socketTransport,
                           std::shared_ptr<TlsServerContext> context)
    : impl(std::make_unique<Impl>()) {
    impl->socketTransport = std::move(socketTransport);
    impl->context = std::move(context);
    if (!impl->socketTransport || !impl->context || !impl->context->impl ||
        impl->context->impl->nativeContext == nullptr) {
        // 底层传输或有效 TLS 上下文缺失，保留不可用的 TLS 连接状态。
        return;
    }
    const std::intptr_t nativeHandle = impl->socketTransport->nativeHandle(); // 原生句柄：绑定 TLS 会话的底层 socket
    if (nativeHandle < 0 || nativeHandle > std::numeric_limits<int>::max()) {
        // 底层 socket 句柄无效或超出 OpenSSL 接口范围，结束 TLS 会话创建。
        return;
    }
    ERR_clear_error();
    impl->session = SSL_new(impl->context->impl->nativeContext);
    if (impl->session == nullptr || SSL_set_fd(impl->session, static_cast<int>(nativeHandle)) != 1) {
        // SSL 会话创建或底层 socket 绑定失败，释放已创建会话并清理错误队列。
        if (impl->session != nullptr) {
            // SSL 会话对象已创建，释放该对象后重置指针。
            SSL_free(impl->session);
            impl->session = nullptr;
        }
        ERR_clear_error();
    }
}

/*
    功能：销毁 TLS 传输并释放会话和底层 socket
    传参：无
    返回值：无
*/
TlsTransport::~TlsTransport() {
    close();
}

/*
    功能：读取 TLS 会话关联的原生 socket 句柄
    传参：无
    返回值：有效 socket 句柄；TLS 会话或传输无效时返回 -1
*/
std::intptr_t TlsTransport::nativeHandle() const noexcept {
    if (impl == nullptr || impl->session == nullptr || !impl->socketTransport) {
        // TLS 实现、会话或底层传输不存在，报告无效句柄。
        return -1;
    }
    return impl->socketTransport->nativeHandle();
}

/*
    功能：推进 TLS 服务端握手
    传参：无
    返回值：握手结果或传输层等待状态
*/
TransportResult TlsTransport::handshake() {
    if (nativeHandle() < 0) {
        // TLS 会话未绑定有效 socket，报告握手失败。
        return TransportResult::FAILED;
    }
    ERR_clear_error();
    const int result = SSL_accept(impl->session); // 握手结果：供 OpenSSL 错误转换逻辑判定
    if (result == 1) {
        // TLS 握手完成，重置后续传输的默认关注方向。
        impl->desiredEvents = WANT_READ;
        return TransportResult::OK;
    }
    return translateSslFailure(impl->session, result, impl->desiredEvents);
}

/*
    功能：从 TLS 会话读取明文并追加到输入缓冲
    传参：buffer：输入缓冲；maxBytes：本次最多读取的字节数
    返回值：传输读取状态
*/
TransportResult TlsTransport::read(ByteBuffer& buffer, std::size_t maxBytes) {
    if (nativeHandle() < 0) {
        // TLS 会话未绑定有效 socket，无法读取明文。
        return TransportResult::FAILED;
    }
    if (!impl->readPending && maxBytes == 0) {
        // 当前没有挂起读操作且读取预算为零，保持可读关注并结束本次调用。
        impl->desiredEvents = WANT_READ;
        return TransportResult::OK;
    }

    if (!impl->readPending) {
        // 当前没有尚未完成的 SSL_read，按预算准备新的读取缓冲。
        const std::size_t amount = std::min({maxBytes, TLS_READ_CHUNK_BYTES,
                                             impl->readRetryBuffer.max_size()}); // 读取长度：本次 TLS 操作允许接收的最大字节数
        if (amount == 0) {
            // 本次 TLS 读取长度为零，无法开始新的读取操作。
            return TransportResult::FAILED;
        }
        try {
            impl->readRetryBuffer.resize(amount);
        } catch (...) {
            // TLS 读取暂存区扩容失败，报告读取失败。
            return TransportResult::FAILED;
        }
        impl->readRetryBytes = amount;
    }

    std::size_t received = 0; // 已读字节数：由 SSL_read_ex 返回的明文长度
    ERR_clear_error();
    const int result = SSL_read_ex(impl->session, impl->readRetryBuffer.data(), impl->readRetryBytes, &received); // 读取结果：供 OpenSSL 错误转换逻辑判定
    if (result == 1) {
        // TLS 明文读取成功，将暂存数据追加到调用方输入缓冲。
        try {
            buffer.insert(buffer.end(), impl->readRetryBuffer.data(), impl->readRetryBuffer.data() + received);
        } catch (...) {
            // 调用方输入缓冲扩容失败，清除挂起读取状态并报告失败。
            impl->readPending = false;
            impl->readRetryBytes = 0;
            impl->readRetryBuffer.clear();
            return TransportResult::FAILED;
        }
        impl->readPending = false;
        impl->readRetryBytes = 0;
        impl->readRetryBuffer.clear();
        impl->desiredEvents = WANT_READ;
        return TransportResult::OK;
    }

    const TransportResult failure = translateSslFailure(impl->session, result, impl->desiredEvents); // 失败状态：区分等待重试、对端关闭和不可恢复错误
    if (failure == TransportResult::WOULD_BLOCK) {
        // OpenSSL 需要底层 I/O 就绪后重试，保留原读取缓冲和长度。
        impl->readPending = true;
    } else {
        // 读取已结束或不可恢复，清除挂起状态及暂存数据。
        impl->readPending = false;
        impl->readRetryBytes = 0;
        impl->readRetryBuffer.clear();
    }
    return failure;
}

/*
    功能：向 TLS 会话写入明文并更新已消费偏移
    传参：buffer：待发送明文字节；offset：已发送位置
    返回值：传输写入状态
*/
TransportResult TlsTransport::write(std::span<const std::uint8_t> buffer, std::size_t& offset) {
    if (nativeHandle() < 0 || offset > buffer.size()) {
        // TLS 会话未绑定有效 socket 或发送偏移越界，拒绝写入。
        return TransportResult::FAILED;
    }
    if (offset == buffer.size()) {
        // 所有明文字节均已发送，恢复默认可读关注。
        impl->desiredEvents = WANT_READ;
        return TransportResult::OK;
    }

    std::size_t sent = 0; // 已写字节数：由 SSL_write_ex 返回的明文消费长度
    ERR_clear_error();
    const int result = SSL_write_ex(impl->session, buffer.data() + offset, buffer.size() - offset, &sent); // 写入结果：供 OpenSSL 错误转换逻辑判定
    if (result == 1) {
        // TLS 明文写入成功，推进偏移并恢复默认可读关注。
        offset += sent;
        impl->desiredEvents = WANT_READ;
        return TransportResult::OK;
    }
    return translateSslFailure(impl->session, result, impl->desiredEvents);
}

/*
    功能：读取 TLS 传输当前要求关注的 I/O 事件
    传参：无
    返回值：当前等待事件；实现对象不存在时返回 WANT_READ
*/
std::uint8_t TlsTransport::events() const {
    return impl == nullptr ? WANT_READ : impl->desiredEvents;
}

/*
    功能：释放 TLS 会话和底层传输并重置重试状态
    传参：无
    返回值：无
*/
void TlsTransport::close() {
    if (impl == nullptr) {
        // 实现对象不存在，无需执行清理。
        return;
    }
    if (impl->session != nullptr) {
        // SSL 会话对象有效，释放会话并清空指针。
        SSL_free(impl->session);
        impl->session = nullptr;
    }
    if (impl->socketTransport) {
        // 底层传输对象存在，关闭其拥有的 socket。
        impl->socketTransport->close();
    }
    impl->readPending = false;
    impl->readRetryBytes = 0;
    impl->readRetryBuffer.clear();
    impl->desiredEvents = WANT_READ;
}

}
