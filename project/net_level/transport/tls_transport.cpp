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

struct TlsServerContext::Impl {
    SSL_CTX* nativeContext = nullptr;              // OpenSSL 上下文：由共享服务端配置创建

    /*
        功能：释放 OpenSSL 服务端上下文
        传参：无
        返回值：无
    */
    ~Impl() {
        if (nativeContext != nullptr) {
            SSL_CTX_free(nativeContext);
        }
    }
};

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

constexpr std::size_t TLS_READ_CHUNK_BYTES = 16 * 1024;

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
    const int sslError = SSL_get_error(session, result);
    if (sslError == SSL_ERROR_WANT_READ) {
        desiredEvents = WANT_READ;
        return TransportResult::WOULD_BLOCK;
    }
    if (sslError == SSL_ERROR_WANT_WRITE) {
        desiredEvents = WANT_WRITE;
        return TransportResult::WOULD_BLOCK;
    }
    if (sslError == SSL_ERROR_ZERO_RETURN) {
        desiredEvents = WANT_READ;
        return TransportResult::PEER_CLOSED;
    }
    desiredEvents = WANT_READ;
    return TransportResult::FAILED;
}

}

TlsServerContext::TlsServerContext(std::unique_ptr<Impl> impl) noexcept : impl(std::move(impl)) {}

std::shared_ptr<TlsServerContext> TlsServerContext::create(const std::string& certificateFile,
                                                           const std::string& privateKeyFile) {
    ERR_clear_error();
    auto contextImpl = std::make_unique<Impl>();
    contextImpl->nativeContext = SSL_CTX_new(TLS_server_method());
    if (contextImpl->nativeContext == nullptr) {
        ERR_clear_error();
        return {};
    }
    SSL_CTX_set_default_passwd_cb(contextImpl->nativeContext, rejectPrivateKeyPassword);
    if (SSL_CTX_set_min_proto_version(contextImpl->nativeContext, TLS1_2_VERSION) != 1 ||
        SSL_CTX_use_certificate_chain_file(contextImpl->nativeContext, certificateFile.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(contextImpl->nativeContext, privateKeyFile.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(contextImpl->nativeContext) != 1) {
        ERR_clear_error();
        return {};
    }
    return std::shared_ptr<TlsServerContext>(new TlsServerContext(std::move(contextImpl)));
}

TlsServerContext::~TlsServerContext() = default;

TlsTransport::TlsTransport(std::unique_ptr<Transport> socketTransport,
                           std::shared_ptr<TlsServerContext> context)
    : impl(std::make_unique<Impl>()) {
    impl->socketTransport = std::move(socketTransport);
    impl->context = std::move(context);
    if (!impl->socketTransport || !impl->context || !impl->context->impl ||
        impl->context->impl->nativeContext == nullptr) {
        return;
    }
    const std::intptr_t nativeHandle = impl->socketTransport->nativeHandle();
    if (nativeHandle < 0 || nativeHandle > std::numeric_limits<int>::max()) {
        return;
    }
    ERR_clear_error();
    impl->session = SSL_new(impl->context->impl->nativeContext);
    if (impl->session == nullptr || SSL_set_fd(impl->session, static_cast<int>(nativeHandle)) != 1) {
        if (impl->session != nullptr) {
            SSL_free(impl->session);
            impl->session = nullptr;
        }
        ERR_clear_error();
    }
}

TlsTransport::~TlsTransport() {
    close();
}

std::intptr_t TlsTransport::nativeHandle() const noexcept {
    if (impl == nullptr || impl->session == nullptr || !impl->socketTransport) {
        return -1;
    }
    return impl->socketTransport->nativeHandle();
}

TransportResult TlsTransport::handshake() {
    if (nativeHandle() < 0) {
        return TransportResult::FAILED;
    }
    ERR_clear_error();
    const int result = SSL_accept(impl->session);
    if (result == 1) {
        impl->desiredEvents = WANT_READ;
        return TransportResult::OK;
    }
    return translateSslFailure(impl->session, result, impl->desiredEvents);
}

TransportResult TlsTransport::read(ByteBuffer& buffer, std::size_t maxBytes) {
    if (nativeHandle() < 0) {
        return TransportResult::FAILED;
    }
    if (!impl->readPending && maxBytes == 0) {
        impl->desiredEvents = WANT_READ;
        return TransportResult::OK;
    }

    if (!impl->readPending) {
        const std::size_t amount = std::min({maxBytes, TLS_READ_CHUNK_BYTES,
                                             impl->readRetryBuffer.max_size()});
        if (amount == 0) {
            return TransportResult::FAILED;
        }
        try {
            impl->readRetryBuffer.resize(amount);
        } catch (...) {
            return TransportResult::FAILED;
        }
        impl->readRetryBytes = amount;
    }

    std::size_t received = 0;
    ERR_clear_error();
    const int result = SSL_read_ex(impl->session, impl->readRetryBuffer.data(), impl->readRetryBytes, &received);
    if (result == 1) {
        try {
            buffer.insert(buffer.end(), impl->readRetryBuffer.data(), impl->readRetryBuffer.data() + received);
        } catch (...) {
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

    const TransportResult failure = translateSslFailure(impl->session, result, impl->desiredEvents);
    if (failure == TransportResult::WOULD_BLOCK) {
        impl->readPending = true;
    } else {
        impl->readPending = false;
        impl->readRetryBytes = 0;
        impl->readRetryBuffer.clear();
    }
    return failure;
}

TransportResult TlsTransport::write(std::span<const std::uint8_t> buffer, std::size_t& offset) {
    if (nativeHandle() < 0 || offset > buffer.size()) {
        return TransportResult::FAILED;
    }
    if (offset == buffer.size()) {
        impl->desiredEvents = WANT_READ;
        return TransportResult::OK;
    }

    std::size_t sent = 0;
    ERR_clear_error();
    const int result = SSL_write_ex(impl->session, buffer.data() + offset, buffer.size() - offset, &sent);
    if (result == 1) {
        offset += sent;
        impl->desiredEvents = WANT_READ;
        return TransportResult::OK;
    }
    return translateSslFailure(impl->session, result, impl->desiredEvents);
}

std::uint8_t TlsTransport::events() const {
    return impl == nullptr ? WANT_READ : impl->desiredEvents;
}

void TlsTransport::close() {
    if (impl == nullptr) {
        return;
    }
    if (impl->session != nullptr) {
        SSL_free(impl->session);
        impl->session = nullptr;
    }
    if (impl->socketTransport) {
        impl->socketTransport->close();
    }
    impl->readPending = false;
    impl->readRetryBytes = 0;
    impl->readRetryBuffer.clear();
    impl->desiredEvents = WANT_READ;
}

}
