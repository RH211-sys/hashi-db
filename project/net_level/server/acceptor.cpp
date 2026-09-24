/*
    模块名：连接接收器
    功能描述：创建 TCP 监听端点，在独立接收线程中接受连接并交给服务端分派。
*/

#include "acceptor.h"
#include "../transport/tcp_transport.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mydb::net {

/*
    函数：Acceptor
    参数：endpoint：监听地址；backlog：监听队列长度；onAccepted：新连接回调
    功能：创建监听接收器配置
    返回：无
*/
Acceptor::Acceptor(Endpoint endpoint, std::uint32_t backlog, AcceptedConnection onAccepted)
    : endpoint(std::move(endpoint)), backlog(backlog), onAccepted(std::move(onAccepted)) {}

/*
    函数：start
    参数：无
    功能：创建监听 socket 并启动 accept 线程
    返回：是否启动成功
*/
bool Acceptor::start() {
    if (backlog == 0 || backlog > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        maxClients == 0 || !onAccepted || activeClients == nullptr) {
        return false;
    }
#ifdef _WIN32
    return false;
#else
    if (running.load(std::memory_order_acquire)) {
        return true;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = endpoint.address.empty() ? AI_PASSIVE : 0;

    addrinfo* addresses = nullptr;
    const auto service = std::to_string(endpoint.port);
    const char* host = endpoint.address.empty() ? nullptr : endpoint.address.c_str();
    if (getaddrinfo(host, service.c_str(), &hints, &addresses) != 0) {
        return false;
    }

    int listener = -1;
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        listener = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (listener < 0) {
            continue;
        }

        const int reuse = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        const int flags = fcntl(listener, F_GETFL, 0);
        if (flags < 0 || fcntl(listener, F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(listener, F_SETFD, FD_CLOEXEC) < 0 ||
            bind(listener, address->ai_addr, address->ai_addrlen) < 0 ||
            listen(listener, static_cast<int>(backlog)) < 0) {
            close(listener);
            listener = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(addresses);

    if (listener < 0) {
        return false;
    }

    listenHandle = listener;
    running.store(true, std::memory_order_release);
    worker = std::thread([this, listener]() {
        pollfd descriptor{};
        descriptor.fd = listener;
        descriptor.events = POLLIN;
        while (running.load(std::memory_order_acquire)) {
            const int ready = poll(&descriptor, 1, 100);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (ready == 0 || (descriptor.revents & POLLIN) == 0) {
                continue;
            }

            while (running.load(std::memory_order_acquire)) {
                sockaddr_storage peerAddress{};
                socklen_t peerLength = sizeof(peerAddress);
                const int client = accept(listener, reinterpret_cast<sockaddr*>(&peerAddress), &peerLength);
                if (client < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    break;
                }

                const int clientFlags = fcntl(client, F_GETFL, 0);
                if (clientFlags < 0 || fcntl(client, F_SETFL, clientFlags | O_NONBLOCK) < 0 ||
                    fcntl(client, F_SETFD, FD_CLOEXEC) < 0) {
                    close(client);
                    continue;
                }

                const int noDelay = 1;
                const int keepAlive = 1;
                setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
                setsockopt(client, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(keepAlive));

                char hostBuffer[NI_MAXHOST]{};
                char serviceBuffer[NI_MAXSERV]{};
                Endpoint peer;
                if (getnameinfo(reinterpret_cast<sockaddr*>(&peerAddress), peerLength,
                                hostBuffer, sizeof(hostBuffer), serviceBuffer, sizeof(serviceBuffer),
                                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
                    peer.address = hostBuffer;
                    peer.port = static_cast<std::uint16_t>(std::strtoul(serviceBuffer, nullptr, 10));
                }

                if (activeClients == nullptr) {
                    close(client);
                    continue;
                }
                std::size_t clients = activeClients->load(std::memory_order_relaxed);
                bool reserved = false;
                while (clients < maxClients) {
                    if (activeClients->compare_exchange_weak(clients, clients + 1,
                            std::memory_order_acq_rel, std::memory_order_relaxed)) {
                        reserved = true;
                        break;
                    }
                }
                if (!reserved) {
                    close(client);
                    continue;
                }

                std::shared_ptr<std::atomic<bool>> released;
                try {
                    released = std::make_shared<std::atomic<bool>>(false);
                } catch (...) {
                    ::close(client);
                    activeClients->fetch_sub(1, std::memory_order_acq_rel);
                    continue;
                }
                const auto releaseSlot = [counter = activeClients, released]() {
                    if (!released->exchange(true, std::memory_order_acq_rel)) {
                        counter->fetch_sub(1, std::memory_order_acq_rel);
                    }
                };
                std::unique_ptr<TcpTransport> transport(new (std::nothrow) TcpTransport(client));
                if (!transport) {
                    ::close(client);
                    releaseSlot();
                    continue;
                }
                if (transport->nativeHandle() < 0) {
                    releaseSlot();
                    continue;
                }
                try {
                    onAccepted(std::move(transport), peer, releaseSlot);
                } catch (...) {
                    releaseSlot();
                }
            }
        }
    });
    return true;
#endif
}

/*
    函数：stop
    参数：无
    功能：停止接收连接并关闭监听资源
    返回：无
*/
void Acceptor::stop() {
#ifndef _WIN32
    running.store(false, std::memory_order_release);
    if (worker.joinable()) {
        worker.join();
    }
    if (listenHandle >= 0) {
        close(static_cast<int>(listenHandle));
        listenHandle = -1;
    }
#endif
}

}
