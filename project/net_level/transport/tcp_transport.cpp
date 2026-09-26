/*
    模块名：TCP 网络传输实现
    模块地位：Transport 抽象的普通 TCP 传输实现。
    模块功能描述：以非阻塞方式读写 TCP 字节流并管理底层 socket。
*/

#include "tcp_transport.h"

#include <algorithm>
#include <cerrno>
#include <limits>

#if defined(__linux__) && !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mydb::net {

/*
    函数：TcpTransport
    传参：nativeHandle：已接受的原生 socket 句柄
    功能：接管 socket 并确保其为非阻塞且设置关闭执行标记
    返回值：无
*/
TcpTransport::TcpTransport(std::intptr_t nativeHandle) noexcept {
#if defined(__linux__) && !defined(_WIN32)
    if (nativeHandle < 0 || nativeHandle > std::numeric_limits<int>::max()) {
        // 原生句柄超出有效范围，保留无效传输状态。
        return;
    }

    const int fd = static_cast<int>(nativeHandle); // 文件描述符：转换后的 Linux socket 句柄
    int flags;                                      // 状态标志：读取并更新 socket 文件状态
    do {
        flags = ::fcntl(fd, F_GETFL, 0);
    } while (flags == -1 && errno == EINTR);

    if (flags == -1) {
        // 无法读取 socket 状态标记，关闭已接管句柄并结束初始化。
        ::close(fd);
        return;
    }

    int result;                                     // 系统调用结果：记录 fcntl 设置状态
    do {
        result = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        // 无法将 socket 设置为非阻塞，关闭句柄并结束初始化。
        ::close(fd);
        return;
    }

    int descriptorFlags;                            // 描述符标志：读取并更新 socket 描述符属性
    do {
        descriptorFlags = ::fcntl(fd, F_GETFD, 0);
    } while (descriptorFlags == -1 && errno == EINTR);
    if (descriptorFlags == -1) {
        // 无法读取 socket 描述符标记，关闭句柄并结束初始化。
        ::close(fd);
        return;
    }

    do {
        result = ::fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        // 无法设置关闭执行标记，关闭句柄并结束初始化。
        ::close(fd);
        return;
    }

    socketHandle = nativeHandle;
    available = true;
#else
    (void)nativeHandle;
#endif
}

/*
    功能：销毁 TCP 传输并关闭其 socket
    传参：无
    返回值：无
*/
TcpTransport::~TcpTransport() {
    close();
}

/*
    功能：读取当前 socket 的原生句柄
    传参：无
    返回值：有效 socket 句柄；传输无效时返回 -1
*/
std::intptr_t TcpTransport::nativeHandle() const noexcept {
    return available ? socketHandle : -1;
}

/*
    功能：完成普通 TCP 传输的握手阶段初始化
    传参：无
    返回值：传输可用时返回 OK，否则返回 FAILED
*/
TransportResult TcpTransport::handshake() {
    if (!available) {
        // socket 不可用，报告握手阶段失败。
        return TransportResult::FAILED;
    }
    desiredEvents = WANT_READ;
    return TransportResult::OK;
}

/*
    功能：从非阻塞 socket 读取字节并追加到输入缓冲
    传参：buffer：输入缓冲；maxBytes：本次最多读取的字节数
    返回值：传输读取状态
*/
TransportResult TcpTransport::read(ByteBuffer& buffer, std::size_t maxBytes) {
#if defined(__linux__) && !defined(_WIN32)
    if (!available) {
        // socket 不可用，无法读取输入数据。
        return TransportResult::FAILED;
    }
    if (maxBytes == 0) {
        // 本次读取预算为零，保留可读关注并结束读取。
        desiredEvents = static_cast<std::uint8_t>(desiredEvents | WANT_READ);
        return TransportResult::OK;
    }

    const std::size_t room = buffer.max_size() - buffer.size(); // 剩余容量：输入缓冲还能扩展的字节数
    const std::size_t amount = std::min({maxBytes, room,
        static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())}); // 读取长度：本次 socket 调用允许接收的最大字节数
    if (amount == 0) {
        // 输入缓冲没有可扩展容量，无法安全接收更多数据。
        return TransportResult::FAILED;
    }

    const std::size_t originalSize = buffer.size(); // 原始长度：读取失败时恢复输入缓冲的边界
    try {
        buffer.resize(originalSize + amount);
    } catch (...) {
        // 输入缓冲扩容失败，报告传输读取失败。
        return TransportResult::FAILED;
    }

    ssize_t received;                               // recv 结果：记录本次读取的字节数或错误状态
    do {
        received = ::recv(static_cast<int>(socketHandle), buffer.data() + originalSize, amount, 0);
    } while (received == -1 && errno == EINTR);

    if (received > 0) {
        // socket 读取到数据，裁剪缓冲区并保留可读关注。
        buffer.resize(originalSize + static_cast<std::size_t>(received));
        desiredEvents = static_cast<std::uint8_t>(desiredEvents | WANT_READ);
        return TransportResult::OK;
    }

    const int receiveError = errno;                 // 读取错误：保存 recv 失败时的系统错误码
    buffer.resize(originalSize);
    if (received == 0) {
        // recv 返回零，表示对端已关闭发送方向。
        return TransportResult::PEER_CLOSED;
    }
    if (receiveError == EAGAIN || receiveError == EWOULDBLOCK) {
        // socket 当前没有可读数据，保留可读关注并报告稍后重试。
        desiredEvents = static_cast<std::uint8_t>(desiredEvents | WANT_READ);
        return TransportResult::WOULD_BLOCK;
    }
    return TransportResult::FAILED;
#else
    (void)buffer;
    (void)maxBytes;
    return TransportResult::FAILED;
#endif
}

/*
    功能：从指定偏移开始向非阻塞 socket 写出字节
    传参：buffer：待发送字节；offset：可更新的已发送偏移量
    返回值：传输写入状态
*/
TransportResult TcpTransport::write(std::span<const std::uint8_t> buffer, std::size_t& offset) {
#if defined(__linux__) && !defined(_WIN32)
    if (!available || offset > buffer.size()) {
        // socket 不可用或发送偏移越界，拒绝写入。
        return TransportResult::FAILED;
    }
    if (offset == buffer.size()) {
        // 所有字节均已发送，切换回可读关注。
        desiredEvents = static_cast<std::uint8_t>((desiredEvents & ~WANT_WRITE) | WANT_READ);
        return TransportResult::OK;
    }

    const std::size_t remaining = buffer.size() - offset; // 剩余长度：当前尚未发送的数据量
    const std::size_t amount = std::min(remaining,
        static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())); // 写入长度：本次 socket 调用允许发送的最大字节数
    ssize_t sent;                                    // send 结果：记录本次写出的字节数或错误状态
    do {
        sent = ::send(static_cast<int>(socketHandle), buffer.data() + offset, amount, MSG_NOSIGNAL);
    } while (sent == -1 && errno == EINTR);

    if (sent > 0) {
        // socket 已写出部分数据，推进偏移并更新后续关注事件。
        offset += static_cast<std::size_t>(sent);
        if (offset < buffer.size()) {
            // 仍有未发送字节，继续关注可写事件。
            desiredEvents = static_cast<std::uint8_t>(desiredEvents | WANT_WRITE);
        } else {
            // 当前缓冲已全部发送，清除可写关注并恢复可读关注。
            desiredEvents = static_cast<std::uint8_t>((desiredEvents & ~WANT_WRITE) | WANT_READ);
        }
        return TransportResult::OK;
    }
    if (sent == 0) {
        // send 未写出字节且未报告可重试状态，按传输失败处理。
        return TransportResult::FAILED;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // socket 暂无可写空间，保留可写关注并报告稍后重试。
        desiredEvents = static_cast<std::uint8_t>(desiredEvents | WANT_WRITE);
        return TransportResult::WOULD_BLOCK;
    }
    return TransportResult::FAILED;
#else
    (void)buffer;
    (void)offset;
    return TransportResult::FAILED;
#endif
}

/*
    功能：读取传输层当前要求的 I/O 关注事件
    传参：无
    返回值：WANT_READ 和 WANT_WRITE 事件位
*/
std::uint8_t TcpTransport::events() const {
    return desiredEvents;
}

/*
    功能：关闭 socket 并重置传输状态
    传参：无
    返回值：无
*/
void TcpTransport::close() {
#if defined(__linux__) && !defined(_WIN32)
    if (available) {
        // socket 当前有效，关闭底层文件描述符。
        ::close(static_cast<int>(socketHandle));
    }
#endif
    socketHandle = -1;
    available = false;
    desiredEvents = WANT_READ;
}

}
