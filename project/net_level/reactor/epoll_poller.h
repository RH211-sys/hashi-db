#pragma once
#ifndef _MYDB_NET_EPOLL_POLLER_H_
#define _MYDB_NET_EPOLL_POLLER_H_

/*
    模块名：epoll 事件轮询器
    功能描述：提供 Linux level-triggered epoll Poller，并以内置 eventfd 唤醒等待者。
*/

#include "poller.h"
#include <cstdint>
#include <unordered_map>

namespace mydb::net {

/*
    类名：EpollPoller
    功能：将连接 ID 映射到原生句柄并通过 epoll 等待就绪事件。
        - level-triggered 监视可读、可写、错误及挂断事件。
        - 使用 eventfd 作为内部唤醒源，唤醒事件的 ConnectionId 为 0。
        - 非 Linux 平台可构造，但操作返回失败。
*/
class EpollPoller final : public Poller {
private:
#if defined(__linux__) && !defined(_WIN32)
    int epollHandle = -1;                          // epoll：Linux epoll 实例句柄
    int wakeHandle = -1;                           // eventfd：线程间唤醒计数器
    std::unordered_map<ConnectionId, std::intptr_t> descriptors; // 连接映射：不以 fd 作为连接标识
#endif

public:
    EpollPoller() noexcept;
    ~EpollPoller() override;

    EpollPoller(const EpollPoller&) = delete;
    EpollPoller& operator=(const EpollPoller&) = delete;
    EpollPoller(EpollPoller&&) = delete;
    EpollPoller& operator=(EpollPoller&&) = delete;

    bool add(ConnectionId connectionId, std::intptr_t nativeHandle, std::uint32_t events) override;
    bool modify(ConnectionId connectionId, std::uint32_t events) override;
    bool remove(ConnectionId connectionId) override;
    int wait(PollEventItem* events, int maxEvents, int timeoutMs) override;
    bool wakeup() override;
};

}

#endif
