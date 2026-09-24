#pragma once
#ifndef _MYDB_NET_POLLER_H_
#define _MYDB_NET_POLLER_H_

/*
    模块名：事件轮询抽象
    功能描述：定义 epoll/IOCP 等平台事件轮询器的最小接口，核心状态机不直接依赖平台 API。
*/

#include "../common/net_types.h"
#include <cstdint>

namespace mydb::net {

/*
    类型名：PollEvent
    功能：定义事件轮询器返回的 I/O 事件位。
*/
enum PollEvent : std::uint32_t {
    POLL_READ = 1U << 0,                          // 可读：socket 有输入数据或握手读事件
    POLL_WRITE = 1U << 1,                         // 可写：socket 可以继续发送数据
    POLL_ERROR = 1U << 2,                         // 错误：底层 socket 发生错误
    POLL_HANGUP = 1U << 3                         // 挂断：对端关闭或连接半关闭
};

/*
    类型名：PollEventItem
    功能：保存一次轮询返回的连接标识和事件位。
*/
struct PollEventItem {
    ConnectionId connectionId = 0;               // 连接 ID：事件所属连接
    std::uint32_t events = 0;                    // 事件位：PollEvent 的组合值
};

/*
    类名：Poller
    功能：定义 epoll、IOCP 或测试 fake poller 的统一事件轮询边界。
        - 管理连接的关注事件。
        - 等待并返回就绪事件。
        - 核心 Reactor 不依赖具体平台 API。
    友元类：无
*/
class Poller {
public:
    /*
        函数：~Poller
        参数：无
        功能：销毁事件轮询器
        返回：无
    */
    virtual ~Poller() = default;

    /*
        函数：add
        参数：connectionId：连接标识；events：关注事件
        功能：将连接加入事件轮询器
        返回：是否加入成功
    */
    virtual bool add(ConnectionId connectionId, std::uint32_t events) = 0;

    /*
        函数：modify
        参数：connectionId：连接标识；events：新的关注事件
        功能：修改连接的事件关注集合
        返回：是否修改成功
    */
    virtual bool modify(ConnectionId connectionId, std::uint32_t events) = 0;

    /*
        函数：remove
        参数：connectionId：连接标识
        功能：从事件轮询器移除连接
        返回：是否移除成功
    */
    virtual bool remove(ConnectionId connectionId) = 0;

    /*
        函数：wait
        参数：events：输出事件数组；maxEvents：数组容量；timeoutMs：等待时间
        功能：等待一批 I/O 事件
        返回：事件数量；失败返回负数
    */
    virtual int wait(PollEventItem* events, int maxEvents, int timeoutMs) = 0;
};

}

#endif
