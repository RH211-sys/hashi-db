#pragma once
#ifndef _MYDB_NET_REACTOR_H_
#define _MYDB_NET_REACTOR_H_

/*
    模块名：Reactor 事件循环
    功能描述：在单线程内驱动 Poller、Connection、跨线程 inbox 和定时任务，保证连接线程亲和性。
*/

#include "poller.h"
#include "../command/command_executor.h"
#include <functional>
#include <memory>

namespace mydb::net {

using ReactorTask = std::function<void()>;      // Reactor 任务：需要在指定事件循环线程执行的闭包

/*
    类名：Reactor
    功能：运行一个事件循环线程并驱动其所属连接。
        - 处理 Poller 返回的读写事件。
        - 执行跨线程投递到 inbox 的任务。
        - 将异步业务完成结果回投到正确的连接。
        - 维护连接线程亲和性。
    友元类：ReactorGroup
*/
class Reactor {
private:
    ReactorId id;                                // Reactor ID：当前事件循环的唯一标识
    std::unique_ptr<Poller> poller;              // 事件轮询器：epoll、IOCP 或 fake 实现
    std::shared_ptr<ICommandExecutor> executor;  // 命令执行器：连接解析后的请求提交目标

public:
    /*
        函数：Reactor
        参数：id：Reactor 标识；poller：事件轮询器；executor：命令执行器
        功能：创建一个 Reactor 及其依赖
        返回：无
    */
    Reactor(ReactorId id, std::unique_ptr<Poller> poller, std::shared_ptr<ICommandExecutor> executor);

    /*
        函数：start
        参数：无
        功能：启动 Reactor 事件循环
        返回：无
    */
    void start();

    /*
        函数：stop
        参数：drain：是否先完成已提交请求
        功能：请求 Reactor 停止接收新请求并进入关闭流程
        返回：无
    */
    void stop(bool drain);

    /*
        函数：post
        参数：task：投递到 Reactor 线程执行的任务
        功能：从其它线程向所属 Reactor 投递连接完成、关闭或控制任务
        返回：是否成功投递
    */
    bool post(ReactorTask task);
};

}

#endif
