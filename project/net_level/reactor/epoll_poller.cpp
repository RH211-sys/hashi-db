/*
    模块名：epoll 事件轮询器实现
    模块地位：Poller 抽象在 Linux 平台上的具体实现。
    模块功能描述：通过 epoll 等待连接事件，并通过 eventfd 唤醒等待线程。
*/

#include "epoll_poller.h"

#include <cerrno>
#include <limits>

#if defined(__linux__) && !defined(_WIN32)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace mydb::net {
namespace {

#if defined(__linux__) && !defined(_WIN32)
/*
    功能：将 Poller 事件掩码转换为 epoll 原生事件掩码
    传参：events：Poller 事件掩码
    返回值：epoll 原生事件掩码
*/
std::uint32_t toNativeEvents(std::uint32_t events) {
    std::uint32_t result = 0;                      // Poll 事件：累积转换后的原生事件掩码
    if ((events & POLL_READ) != 0) {
        // Poller 要求监听可读事件，将其加入 epoll 输入事件掩码。
        result |= EPOLLIN;
    }
    if ((events & POLL_WRITE) != 0) {
        // Poller 要求监听可写事件，将其加入 epoll 输出事件掩码。
        result |= EPOLLOUT;
    }
    return result;
}

/*
    功能：将 epoll 原生事件掩码转换为 Poller 事件掩码
    传参：events：epoll 原生事件掩码
    返回值：Poller 事件掩码
*/
std::uint32_t fromNativeEvents(std::uint32_t events) {
    std::uint32_t result = 0;                      // 事件掩码：累积转换后的 Poller 事件类型
    if ((events & EPOLLIN) != 0) {
        // epoll 报告可读事件，将其映射为 Poller 读事件。
        result |= POLL_READ;
    }
    if ((events & EPOLLOUT) != 0) {
        // epoll 报告可写事件，将其映射为 Poller 写事件。
        result |= POLL_WRITE;
    }
    if ((events & EPOLLERR) != 0) {
        // epoll 报告错误事件，将其映射为 Poller 错误事件。
        result |= POLL_ERROR;
    }
    if ((events & (EPOLLHUP | EPOLLRDHUP)) != 0) {
        // epoll 报告挂断或对端半关闭，将其映射为 Poller 挂断事件。
        result |= POLL_HANGUP;
    }
    return result;
}

/*
    功能：执行 epoll_ctl 并在系统调用被信号中断时重试
    传参：epollFd：epoll 实例句柄；operation：控制操作；fd：目标文件描述符；event：事件配置
    返回值：epoll_ctl 的返回值
*/
int epollControl(int epollFd, int operation, int fd, epoll_event* event) {
    int result;                                    // 系统调用结果：保存 epoll_ctl 的返回状态
    do {
        result = ::epoll_ctl(epollFd, operation, fd, event);
    } while (result == -1 && errno == EINTR);
    return result;
}
#endif

}

/*
    功能：创建 epoll 实例和唤醒事件描述符，并注册唤醒事件
    传参：无
    返回值：无
*/
EpollPoller::EpollPoller() noexcept {
#if defined(__linux__) && !defined(_WIN32)
    epollHandle = ::epoll_create1(EPOLL_CLOEXEC);
    if (epollHandle == -1) {
        // epoll 实例创建失败，保留无效状态并结束初始化。
        return;
    }

    wakeHandle = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeHandle == -1) {
        // 唤醒描述符创建失败，关闭已创建的 epoll 实例并恢复无效状态。
        ::close(epollHandle);
        epollHandle = -1;
        return;
    }

    epoll_event event{};                           // 唤醒事件：配置 eventfd 的 epoll 监听项
    event.events = EPOLLIN;
    event.data.u64 = 0;
    if (epollControl(epollHandle, EPOLL_CTL_ADD, wakeHandle, &event) == -1) {
        // 唤醒事件注册失败，关闭两个描述符并恢复无效状态。
        ::close(wakeHandle);
        ::close(epollHandle);
        wakeHandle = -1;
        epollHandle = -1;
    }
#endif
}

/*
    功能：关闭 epoll 实例和唤醒事件描述符
    传参：无
    返回值：无
*/
EpollPoller::~EpollPoller() {
#if defined(__linux__) && !defined(_WIN32)
    if (wakeHandle != -1) {
        // 唤醒描述符有效，关闭该描述符并释放系统资源。
        ::close(wakeHandle);
    }
    if (epollHandle != -1) {
        // epoll 描述符有效，关闭该描述符并释放系统资源。
        ::close(epollHandle);
    }
#endif
}

/*
    功能：把连接描述符加入 epoll 并记录连接标识映射
    传参：connectionId：连接标识；nativeHandle：原生描述符；events：关注事件
    返回值：添加成功时返回 true，否则返回 false
*/
bool EpollPoller::add(ConnectionId connectionId, std::intptr_t nativeHandle, std::uint32_t events) {
#if defined(__linux__) && !defined(_WIN32)
    if (epollHandle == -1 || connectionId == 0 || nativeHandle < 0 ||
        nativeHandle > std::numeric_limits<int>::max() || descriptors.contains(connectionId)) {
        // epoll 未初始化、标识或描述符无效、描述符越界或连接已登记时拒绝添加。
        return false;
    }

    const auto [entry, inserted] = descriptors.emplace(connectionId, nativeHandle); // 插入结果：记录连接描述符项及是否成功插入
    if (!inserted) {
        // 连接标识映射未能插入，报告添加失败。
        return false;
    }

    epoll_event event{};                           // 连接事件：配置当前连接在 epoll 中的监听项
    event.events = toNativeEvents(events) | EPOLLRDHUP;
    event.data.u64 = connectionId;
    if (epollControl(epollHandle, EPOLL_CTL_ADD, static_cast<int>(nativeHandle), &event) == -1) {
        // epoll 注册失败，回滚刚插入的连接映射并报告失败。
        descriptors.erase(entry);
        return false;
    }
    return true;
#else
    (void)connectionId;
    (void)nativeHandle;
    (void)events;
    return false;
#endif
}

/*
    功能：修改已登记连接在 epoll 中的关注事件
    传参：connectionId：连接标识；events：新的关注事件
    返回值：修改成功时返回 true，否则返回 false
*/
bool EpollPoller::modify(ConnectionId connectionId, std::uint32_t events) {
#if defined(__linux__) && !defined(_WIN32)
    if (epollHandle == -1 || connectionId == 0) {
        // epoll 未初始化或连接标识无效，拒绝修改监听事件。
        return false;
    }
    const auto entry = descriptors.find(connectionId); // 描述符项：查找连接对应的原生句柄
    if (entry == descriptors.end()) {
        // 连接未登记到 epoll，无法修改其监听事件。
        return false;
    }

    epoll_event event{};                           // 更新事件：配置连接修改后的 epoll 监听项
    event.events = toNativeEvents(events) | EPOLLRDHUP;
    event.data.u64 = connectionId;
    return epollControl(epollHandle, EPOLL_CTL_MOD, static_cast<int>(entry->second), &event) == 0;
#else
    (void)connectionId;
    (void)events;
    return false;
#endif
}

/*
    功能：从 epoll 中移除连接并删除连接描述符映射
    传参：connectionId：待移除连接标识
    返回值：移除成功或描述符已失效时返回 true，否则返回 false
*/
bool EpollPoller::remove(ConnectionId connectionId) {
#if defined(__linux__) && !defined(_WIN32)
    if (epollHandle == -1 || connectionId == 0) {
        // epoll 未初始化或连接标识无效，拒绝移除连接。
        return false;
    }
    const auto entry = descriptors.find(connectionId); // 描述符项：查找待移除连接对应的原生句柄
    if (entry == descriptors.end()) {
        // 连接未登记到 epoll，无需执行移除操作。
        return false;
    }

    const int fd = static_cast<int>(entry->second); // 文件描述符：转换为 epoll_ctl 所需的 socket 句柄类型
    if (epollControl(epollHandle, EPOLL_CTL_DEL, fd, nullptr) == 0 || errno == ENOENT || errno == EBADF) {
        // epoll 已移除描述符或描述符早已失效，删除本地映射并报告成功。
        descriptors.erase(entry);
        return true;
    }
    return false;
#else
    (void)connectionId;
    return false;
#endif
}

/*
    功能：等待 epoll 事件并转换为 Poller 事件项
    传参：events：输出事件数组；maxEvents：数组容量；timeoutMs：最大等待毫秒数
    返回值：写入的事件数量；参数无效或系统调用失败时返回 -1
*/
int EpollPoller::wait(PollEventItem* events, int maxEvents, int timeoutMs) {
#if defined(__linux__) && !defined(_WIN32)
    if (epollHandle == -1 || events == nullptr || maxEvents <= 0) {
        // epoll 未初始化或输出数组参数无效，返回调用失败。
        return -1;
    }
    epoll_event nativeEvents[128];                  // 原生事件数组：接收 epoll_wait 返回的就绪事件
    const int capacity = maxEvents < 128 ? maxEvents : 128; // 实际容量：限制本轮原生事件数组的接收数量
    int count;                                      // 原生事件数：保存 epoll_wait 返回的就绪事件数量
    do {
        count = ::epoll_wait(epollHandle, nativeEvents, capacity, timeoutMs);
    } while (count == -1 && errno == EINTR);
    if (count == -1) {
        // epoll_wait 遇到不可恢复错误，返回调用失败。
        return -1;
    }

    int outputCount = 0;                            // 输出事件数：记录已转换并写入 Poller 数组的事件数量
    for (int i = 0; i < count; ++i) {                // 原生事件下标：依次转换本轮返回的 epoll 事件
        const ConnectionId id = nativeEvents[i].data.u64; // 连接标识：读取原生事件关联的连接或唤醒标识
        if (id == 0) {
            // 零标识保留给 eventfd，读取计数以清除唤醒通知。
            std::uint64_t value;                    // 唤醒计数：接收 eventfd 中待清除的计数值
            ssize_t readCount;                      // 读取结果：保存 eventfd 计数读取的字节数或错误码
            do {
                readCount = ::read(wakeHandle, &value, sizeof(value));
            } while (readCount == -1 && errno == EINTR);
            if (readCount == static_cast<ssize_t>(sizeof(value)) ||
                (readCount == -1 && errno == EAGAIN)) {
                // 唤醒计数读取成功或计数已被清空，输出 Poller 唤醒事件。
                events[outputCount++] = PollEventItem{0, POLL_WAKE};
            } else {
                // 唤醒计数读取发生不可恢复错误，返回调用失败。
                return -1;
            }
            continue;
        }
        events[outputCount++] = PollEventItem{id, fromNativeEvents(nativeEvents[i].events)};
    }
    return outputCount;
#else
    (void)events;
    (void)maxEvents;
    (void)timeoutMs;
    return -1;
#endif
}

/*
    功能：写入 eventfd 以唤醒正在等待事件的线程
    传参：无
    返回值：唤醒通知写入成功或通知已待处理时返回 true，否则返回 false
*/
bool EpollPoller::wakeup() {
#if defined(__linux__) && !defined(_WIN32)
    if (wakeHandle == -1) {
        // 唤醒描述符无效，无法通知等待线程。
        return false;
    }
    const std::uint64_t value = 1;                  // 唤醒增量：写入 eventfd 的计数值
    ssize_t written;                                // 写入结果：保存 eventfd 写入的字节数或错误码
    do {
        written = ::write(wakeHandle, &value, sizeof(value));
    } while (written == -1 && errno == EINTR);
    return written == static_cast<ssize_t>(sizeof(value)) || (written == -1 && errno == EAGAIN);
#else
    return false;
#endif
}

}
