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

TcpTransport::TcpTransport(std::intptr_t nativeHandle) noexcept {
#if defined(__linux__) && !defined(_WIN32)
    if (nativeHandle < 0 || nativeHandle > std::numeric_limits<int>::max()) {
        return;
    }

    const int fd = static_cast<int>(nativeHandle);
    int flags;
    do {
        flags = ::fcntl(fd, F_GETFL, 0);
    } while (flags == -1 && errno == EINTR);

    if (flags == -1) {
        ::close(fd);
        return;
    }

    int result;
    do {
        result = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        ::close(fd);
        return;
    }

    int descriptorFlags;
    do {
        descriptorFlags = ::fcntl(fd, F_GETFD, 0);
    } while (descriptorFlags == -1 && errno == EINTR);
    if (descriptorFlags == -1) {
        ::close(fd);
        return;
    }

    do {
        result = ::fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        ::close(fd);
        return;
    }

    socketHandle = nativeHandle;
    available = true;
#else
    (void)nativeHandle;
#endif
}

TcpTransport::~TcpTransport() {
    close();
}

std::intptr_t TcpTransport::nativeHandle() const noexcept {
    return available ? socketHandle : -1;
}

TransportResult TcpTransport::handshake() {
    if (!available) {
        return TransportResult::FAILED;
    }
    desiredEvents = WANT_READ;
    return TransportResult::OK;
}

TransportResult TcpTransport::read(ByteBuffer& buffer, std::size_t maxBytes) {
#if defined(__linux__) && !defined(_WIN32)
    if (!available) {
        return TransportResult::FAILED;
    }
    if (maxBytes == 0) {
        desiredEvents = static_cast<std::uint8_t>(desiredEvents | WANT_READ);
        return TransportResult::OK;
    }

    const std::size_t room = buffer.max_size() - buffer.size();
    const std::size_t amount = std::min({maxBytes, room,
        static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())});
    if (amount == 0) {
        return TransportResult::FAILED;
    }

    const std::size_t originalSize = buffer.size();
    try {
        buffer.resize(originalSize + amount);
    } catch (...) {
        return TransportResult::FAILED;
    }

    ssize_t received;
    do {
        received = ::recv(static_cast<int>(socketHandle), buffer.data() + originalSize, amount, 0);
    } while (received == -1 && errno == EINTR);

    if (received > 0) {
        buffer.resize(originalSize + static_cast<std::size_t>(received));
        desiredEvents = static_cast<std::uint8_t>(desiredEvents | WANT_READ);
        return TransportResult::OK;
    }

    const int receiveError = errno;
    buffer.resize(originalSize);
    if (received == 0) {
        return TransportResult::PEER_CLOSED;
    }
    if (receiveError == EAGAIN || receiveError == EWOULDBLOCK) {
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

TransportResult TcpTransport::write(const ByteBuffer& buffer, std::size_t& offset) {
#if defined(__linux__) && !defined(_WIN32)
    if (!available || offset > buffer.size()) {
        return TransportResult::FAILED;
    }
    if (offset == buffer.size()) {
        desiredEvents = static_cast<std::uint8_t>((desiredEvents & ~WANT_WRITE) | WANT_READ);
        return TransportResult::OK;
    }

    const std::size_t remaining = buffer.size() - offset;
    const std::size_t amount = std::min(remaining,
        static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    ssize_t sent;
    do {
        sent = ::send(static_cast<int>(socketHandle), buffer.data() + offset, amount, MSG_NOSIGNAL);
    } while (sent == -1 && errno == EINTR);

    if (sent > 0) {
        offset += static_cast<std::size_t>(sent);
        if (offset < buffer.size()) {
            desiredEvents = static_cast<std::uint8_t>(desiredEvents | WANT_WRITE);
        } else {
            desiredEvents = static_cast<std::uint8_t>((desiredEvents & ~WANT_WRITE) | WANT_READ);
        }
        return TransportResult::OK;
    }
    if (sent == 0) {
        return TransportResult::FAILED;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
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

std::uint8_t TcpTransport::events() const {
    return desiredEvents;
}

void TcpTransport::close() {
#if defined(__linux__) && !defined(_WIN32)
    if (available) {
        ::close(static_cast<int>(socketHandle));
    }
#endif
    socketHandle = -1;
    available = false;
    desiredEvents = WANT_READ;
}

}
