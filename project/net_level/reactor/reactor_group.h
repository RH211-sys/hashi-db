#pragma once
#ifndef _MYDB_NET_REACTOR_GROUP_H_
#define _MYDB_NET_REACTOR_GROUP_H_

/*
    模块名：Reactor 组
    功能描述：创建和管理多个 Reactor，负责新连接分派、线程生命周期和统一停止，不处理具体命令。
*/

#include "reactor.h"
#include "../server/connection.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace mydb::net {

/*
    类名：ReactorGroup
    功能：统一管理多个 Reactor 及其连接分派。
        - 创建和启动多个事件循环。
        - 将新连接固定绑定到一个 Reactor。
        - 统一执行优雅停止或立即停止。
    友元类：NetworkServer
*/
class ReactorGroup {
private:
    std::vector<std::unique_ptr<Reactor>> reactors; // Reactor 列表：服务端所有事件循环
    std::size_t nextReactor = 0;                   // 分派游标：轮询选择下一个 Reactor

    friend class NetworkServer;                    // NetworkServer：控制 ReactorGroup 生命周期

public:
    /*
        函数：ReactorGroup
        参数：reactorCount：Reactor 数量；executor：共享命令执行器
        功能：创建 Reactor 组配置
        返回：无
    */
    ReactorGroup(std::size_t reactorCount, std::shared_ptr<ICommandExecutor> executor);

    /*
        函数：start
        参数：无
        功能：启动所有 Reactor 线程
        返回：是否全部启动成功
    */
    bool start();

    /*
        函数：stop
        参数：drain：是否优雅排空已提交请求
        功能：停止所有 Reactor 并等待其线程退出
        返回：无
    */
    void stop(bool drain);

    /*
        函数：dispatch
        参数：transport：已接受的非阻塞传输；peer：对端地址；maxPipelineRequests：流水线上限
        功能：将新连接绑定到一个 Reactor
        返回：是否成功分派
    */
    bool dispatch(std::unique_ptr<Transport> transport, Endpoint peer, std::size_t maxPipelineRequests);
};

}

#endif
