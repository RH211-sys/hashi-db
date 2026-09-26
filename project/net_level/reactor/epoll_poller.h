#pragma once
#ifndef _MYDB_NET_EPOLL_POLLER_H_
#define _MYDB_NET_EPOLL_POLLER_H_

/*
    模块名：epoll 事件轮询器
    模块地位：Poller 抽象在 Linux 上基于 epoll 和 eventfd 的实现。
    模块功能描述：提供 Linux level-triggered epoll Poller，并以内置 eventfd 唤醒等待者。
*/

#include "poller.h"
#include <cstdint>
#include <unordered_map>

namespace mydb::net {

/*
    类名：EpollPoller
    地位：Poller 抽象在 Linux 上基于 epoll 和 eventfd 的实现。
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
    /*
        函数：EpollPoller
        传参：无
        功能：创建 epoll 实例和内部唤醒源
        返回值：无
    */
    EpollPoller() noexcept;

    /*
        函数：~EpollPoller
        传参：无
        功能：关闭 epoll 实例和内部唤醒源
        返回值：无
    */
    ~EpollPoller() override;

    /*
        函数：EpollPoller 复制构造
        传参：源对象：待复制的 epoll 轮询器
        功能：禁止复制并共享 epoll 与 eventfd 句柄
        返回值：无
    */
    EpollPoller(const EpollPoller&) = delete;
    /*
        函数：EpollPoller 复制赋值
        传参：源对象：待复制赋值的 epoll 轮询器
        功能：禁止共享或替换 epoll 与 eventfd 句柄
        返回值：赋值目标引用类型；该函数已删除，不可调用
    */
    EpollPoller& operator=(const EpollPoller&) = delete;
    /*
        函数：EpollPoller 移动构造
        传参：源对象：待移动的 epoll 轮询器
        功能：禁止转移由当前对象管理的系统句柄
        返回值：无
    */
    EpollPoller(EpollPoller&&) = delete;
    /*
        函数：EpollPoller 移动赋值
        传参：源对象：待移动赋值的 epoll 轮询器
        功能：禁止替换由当前对象管理的系统句柄
        返回值：赋值目标引用类型；该函数已删除，不可调用
    */
    EpollPoller& operator=(EpollPoller&&) = delete;

    /*
        函数：add
        传参：connectionId：连接标识；nativeHandle：原生句柄；events：关注事件位
        功能：将连接句柄加入 epoll 监视集合
        返回值：是否注册成功
    */
    bool add(ConnectionId connectionId, std::intptr_t nativeHandle, std::uint32_t events) override;

    /*
        函数：modify
        传参：connectionId：连接标识；events：新的关注事件位
        功能：更新连接在 epoll 中的关注事件
        返回值：是否更新成功
    */
    bool modify(ConnectionId connectionId, std::uint32_t events) override;

    /*
        函数：remove
        传参：connectionId：连接标识
        功能：从 epoll 监视集合移除连接
        返回值：是否移除成功
    */
    bool remove(ConnectionId connectionId) override;

    /*
        函数：wait
        传参：events：事件输出缓冲；maxEvents：缓冲容量；timeoutMs：等待超时
        功能：等待并读取一批就绪事件
        返回值：事件数量；发生错误时返回负值
    */
    int wait(PollEventItem* events, int maxEvents, int timeoutMs) override;

    /*
        函数：wakeup
        传参：无
        功能：通过内部 eventfd 唤醒正在等待事件的线程
        返回值：是否成功写入唤醒信号
    */
    bool wakeup() override;
};

}

#endif
