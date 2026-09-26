/*
    模块名：连接接收器
    模块地位：实现监听端点创建、连接接收和新连接交接。
    模块功能描述：创建 TCP 监听端点，在独立接收线程中接受连接并交给服务端分派。
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
    传参：endpoint：监听地址；backlog：监听队列长度；onAccepted：新连接回调
    功能：创建监听接收器配置
    返回值：无
*/
Acceptor::Acceptor(Endpoint endpoint, std::uint32_t backlog, AcceptedConnection onAccepted)
    : endpoint(std::move(endpoint)), backlog(backlog), onAccepted(std::move(onAccepted)) {}

/*
    函数：start
    传参：无
    功能：创建监听 socket 并启动 accept 线程
    返回值：是否启动成功
*/
bool Acceptor::start() {
    if (backlog == 0 || backlog > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        maxClients == 0 || !onAccepted || activeClients == nullptr) {
        // 监听队列、客户端上限、接入回调或计数器无效，拒绝启动接收器。
        return false;
    }
#ifdef _WIN32
    return false;
#else
    if (running.load(std::memory_order_acquire)) {
        // 接收线程已经运行，重复启动视为成功。
        return true;
    }

    addrinfo hints{};                               // 地址提示：指定 TCP 监听地址解析条件
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = endpoint.address.empty() ? AI_PASSIVE : 0;

    addrinfo* addresses = nullptr;                  // 地址列表：接收系统解析得到的候选监听地址
    const auto service = std::to_string(endpoint.port); // 服务端口：转换为地址解析器要求的文本形式
    const char* host = endpoint.address.empty() ? nullptr : endpoint.address.c_str(); // 绑定主机：空地址表示监听所有本地地址
    if (getaddrinfo(host, service.c_str(), &hints, &addresses) != 0) {
        // 监听地址解析失败，无法创建可用的监听 socket。
        return false;
    }

    int listener = -1;                              // 监听句柄：保存成功创建并绑定的 socket
    for (auto* address = addresses; address != nullptr; address = address->ai_next) { // 地址项：逐个尝试解析得到的监听地址
        listener = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (listener < 0) {
            // 当前候选地址无法创建 socket，尝试下一个候选地址。
            continue;
        }

        const int reuse = 1;                         // 地址复用选项：允许监听端点在重启后复用本地地址
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        const int flags = fcntl(listener, F_GETFL, 0); // 描述符标记：读取监听 socket 当前状态标记
        if (flags < 0 || fcntl(listener, F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(listener, F_SETFD, FD_CLOEXEC) < 0 ||
            bind(listener, address->ai_addr, address->ai_addrlen) < 0 ||
            listen(listener, static_cast<int>(backlog)) < 0) {
            // socket 配置、绑定或监听失败，关闭当前候选并继续尝试。
            close(listener);
            listener = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(addresses);

    if (listener < 0) {
        // 所有候选地址均创建失败，报告监听启动失败。
        return false;
    }

    listenHandle = listener;
    running.store(true, std::memory_order_release);
    worker = std::thread([this, listener]() {
        pollfd descriptor{};                         // 监听事件：配置 poll 等待的监听 socket
        descriptor.fd = listener;
        descriptor.events = POLLIN;
        while (running.load(std::memory_order_acquire)) {
            const int ready = poll(&descriptor, 1, 100); // 就绪结果：本轮监听 socket 的 poll 状态
            if (ready < 0) {
                // poll 返回错误，区分信号中断与不可恢复错误。
                if (errno == EINTR) {
                    // poll 被信号中断，继续检查监听 socket 和停止状态。
                    continue;
                }
                // poll 遇到不可恢复错误，结束接收线程。
                break;
            }
            if (ready == 0 || (descriptor.revents & POLLIN) == 0) {
                // 本轮超时或监听 socket 尚不可读，等待下一轮就绪通知。
                continue;
            }

            while (running.load(std::memory_order_acquire)) {
                sockaddr_storage peerAddress{};       // 对端地址：接收 accept 返回的客户端 socket 地址
                socklen_t peerLength = sizeof(peerAddress); // 地址长度：接收并约束对端地址结构长度
                const int client = accept(listener, reinterpret_cast<sockaddr*>(&peerAddress), &peerLength); // 客户端句柄：保存新接受的 socket
                if (client < 0) {
                    // accept 未取得客户端句柄，按错误类型重试或结束本轮接收。
                    if (errno == EINTR) {
                        // accept 被信号中断，继续尝试接受连接。
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // 当前已无待接受连接，结束本轮 accept 排空。
                        break;
                    }
                    // accept 遇到其他错误，结束本轮 accept 排空。
                    break;
                }

                const int clientFlags = fcntl(client, F_GETFL, 0); // 客户端标记：读取新 socket 当前状态标记
                if (clientFlags < 0 || fcntl(client, F_SETFL, clientFlags | O_NONBLOCK) < 0 ||
                    fcntl(client, F_SETFD, FD_CLOEXEC) < 0) {
                    // 新 socket 无法配置为非阻塞或设置关闭执行标记，关闭并跳过。
                    close(client);
                    continue;
                }

                const int noDelay = 1;                // TCP_NODELAY 选项：关闭 Nagle 缓冲以降低交互延迟
                const int keepAlive = 1;              // SO_KEEPALIVE 选项：启用 TCP 连接存活探测
                setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
                setsockopt(client, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(keepAlive));

                char hostBuffer[NI_MAXHOST]{};        // 主机缓冲：接收数字格式的对端地址
                char serviceBuffer[NI_MAXSERV]{};     // 服务缓冲：接收数字格式的对端端口
                Endpoint peer;                        // 对端端点：保存传递给后续连接层的地址信息
                if (getnameinfo(reinterpret_cast<sockaddr*>(&peerAddress), peerLength,
                                hostBuffer, sizeof(hostBuffer), serviceBuffer, sizeof(serviceBuffer),
                                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
                    // 对端地址转换成功，填充连接使用的数字地址和端口。
                    peer.address = hostBuffer;
                    peer.port = static_cast<std::uint16_t>(std::strtoul(serviceBuffer, nullptr, 10));
                }

                if (activeClients == nullptr) {
                    // 客户端计数器不可用，关闭新 socket 并跳过接入。
                    close(client);
                    continue;
                }
                std::size_t clients = activeClients->load(std::memory_order_relaxed); // 客户端数量：读取当前已占用的接入槽位数
                bool reserved = false;               // 槽位结果：标记是否成功预留一个客户端槽位
                while (clients < maxClients) {
                    if (activeClients->compare_exchange_weak(clients, clients + 1,
                            std::memory_order_acq_rel, std::memory_order_relaxed)) {
                        // 原子计数更新成功，当前接入已占用一个客户端槽位。
                        reserved = true;
                        break;
                    }
                }
                if (!reserved) {
                    // 客户端上限已满，关闭新 socket 并拒绝接入。
                    close(client);
                    continue;
                }

                std::shared_ptr<std::atomic<bool>> released; // 释放标记：确保槽位回调至多归还一次
                try {
                    released = std::make_shared<std::atomic<bool>>(false);
                } catch (...) {
                    // 槽位释放标记分配失败，关闭 socket 并直接归还已预留槽位。
                    ::close(client);
                    activeClients->fetch_sub(1, std::memory_order_acq_rel);
                    continue;
                }
                const auto releaseSlot = [counter = activeClients, released]() { // 释放回调：原子地归还客户端槽位且避免重复执行
                    if (!released->exchange(true, std::memory_order_acq_rel)) {
                        // 首次触发槽位释放，递减共享客户端计数。
                        counter->fetch_sub(1, std::memory_order_acq_rel);
                    }
                };
                std::unique_ptr<TcpTransport> transport(new (std::nothrow) TcpTransport(client)); // 新连接传输：接管客户端 socket 的所有权
                if (!transport) {
                    // TCP 传输对象分配失败，关闭 socket 并释放预留槽位。
                    ::close(client);
                    releaseSlot();
                    continue;
                }
                if (transport->nativeHandle() < 0) {
                    // 传输对象未能接管有效 socket，释放槽位并跳过连接。
                    releaseSlot();
                    continue;
                }
                try {
                    // 传输对象和槽位回调准备完成，交由上层接入回调分派连接。
                    onAccepted(std::move(transport), peer, releaseSlot);
                } catch (...) {
                    // 上层接入回调抛出异常，释放本次占用的客户端槽位。
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
    传参：无
    功能：停止接收连接并关闭监听资源
    返回值：无
*/
void Acceptor::stop() {
#ifndef _WIN32
    running.store(false, std::memory_order_release);
    if (worker.joinable()) {
        // 接收线程仍可回收，等待其退出后再释放监听描述符。
        worker.join();
    }
    if (listenHandle >= 0) {
        // 监听描述符有效，关闭并重置句柄。
        close(static_cast<int>(listenHandle));
        listenHandle = -1;
    }
#endif
}

}
