#pragma once
#ifndef _MYDB_NET_REACTOR_H_
#define _MYDB_NET_REACTOR_H_

/*
    模块名：Reactor 事件循环
    模块地位：ReactorGroup 与 Connection 之间的单线程事件调度模块。
    模块功能描述：在单线程内驱动 Poller、Connection 和跨线程 inbox，保证连接线程亲和性。
*/

#include "poller.h"
#include "../command/command_executor.h"
#include "../protocol/protocol.h"
#include "../server/acceptor.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace mydb::net {

class Connection;
using ReactorTask = std::function<void()>;          // Reactor 任务：在对应事件线程中执行的无参可调用对象

/*
    类名：Reactor
    地位：一组连接的单线程事件循环和任务执行者。
    功能：通过 Poller 驱动连接 I/O，并串行处理跨线程投递的任务。
*/
class Reactor {
private:
    struct Impl;                                     // 内部实现类型：隐藏事件线程、任务队列和连接表
    std::shared_ptr<Impl> impl;                      // 内部实现：保存事件循环及其线程安全队列

public:
    /*
        函数：Reactor
        传参：id：Reactor 标识；poller：事件轮询器；executor：连接共享的命令执行器
        功能：创建事件循环并接管其运行依赖
        返回值：无
    */
    Reactor(ReactorId id, std::unique_ptr<Poller> poller, std::shared_ptr<ICommandExecutor> executor);

    /*
        函数：~Reactor
        传参：无
        功能：停止事件循环并释放其资源
        返回值：无
    */
    ~Reactor();

    /*
        函数：Reactor 复制构造
        传参：源对象：待复制的事件循环
        功能：禁止复制线程、任务队列和连接表的所有权
        返回值：无
    */
    Reactor(const Reactor&) = delete;
    /*
        函数：Reactor 复制赋值
        传参：源对象：待复制赋值的事件循环
        功能：禁止替换线程、任务队列和连接表的所有权
        返回值：赋值目标引用类型；该函数已删除，不可调用
    */
    Reactor& operator=(const Reactor&) = delete;
    /*
        函数：Reactor 移动构造
        传参：源对象：待移动的事件循环
        功能：禁止转移事件线程所依赖的 Reactor 状态
        返回值：无
    */
    Reactor(Reactor&&) = delete;
    /*
        函数：Reactor 移动赋值
        传参：源对象：待移动赋值的事件循环
        功能：禁止替换正在管理线程和连接的 Reactor 状态
        返回值：赋值目标引用类型；该函数已删除，不可调用
    */
    Reactor& operator=(Reactor&&) = delete;

    /*
        函数：start
        传参：无
        功能：启动 Reactor 事件线程
        返回值：无
    */
    void start();

    /*
        函数：stop
        传参：无
        功能：请求 Reactor 停止并唤醒事件线程
        返回值：无
    */
    void stop();

    /*
        函数：post
        传参：task：提交到事件线程执行的任务
        功能：向 Reactor 通用任务队列提交任务
        返回值：任务是否成功入队
    */
    bool post(ReactorTask task);

    /*
        函数：addConnection
        传参：connectionId：连接标识；transport：连接传输；peer：对端地址；maxPipelineRequests：流水线上限；releaseSlot：连接槽位释放回调；maxFrameBytes：帧大小上限
        功能：向当前 Reactor 注册并创建连接
        返回值：连接是否成功接入
    */
    bool addConnection(ConnectionId connectionId, std::unique_ptr<Transport> transport, Endpoint peer,
                       std::size_t maxPipelineRequests, ClientSlotRelease releaseSlot = {},
                       std::uint32_t maxFrameBytes = DEFAULT_MAX_FRAME_BYTES);
    /*
        函数：closeConnection
        传参：connectionId：待关闭连接的标识
        功能：向当前 Reactor 请求关闭指定连接
        返回值：关闭任务是否成功提交
    */
    bool closeConnection(ConnectionId connectionId);

    /*
        函数：getId
        传参：无
        功能：读取当前 Reactor 标识
        返回值：当前 Reactor 标识
    */
    ReactorId getId() const noexcept;
};

}

#endif
