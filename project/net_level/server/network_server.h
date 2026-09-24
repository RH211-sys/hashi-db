#pragma once
#ifndef _MYDB_NET_NETWORK_SERVER_H_
#define _MYDB_NET_NETWORK_SERVER_H_

/*
    模块名：网络服务端
    功能描述：组装 Acceptor、ReactorGroup、连接注册表和命令执行器，提供服务生命周期内部实现。
*/

#include "../common/net_types.h"
#include "../command/command_executor.h"
#include "../protocol/protocol.h"
#include "../reactor/reactor_group.h"
#include "acceptor.h"
#include <atomic>
#include <cstddef>
#include <memory>

namespace mydb::net {

/*
    类型名：ServerConfig
    功能：保存网络服务端的监听、线程、连接、帧大小和 TLS 配置。
*/
struct ServerConfig {
    Endpoint listenEndpoint{"127.0.0.1", 6380}; // 监听端点：默认只监听本机 6380 端口
    std::size_t reactorCount = 0;                // Reactor 数量：0 表示按机器逻辑核数自动计算
    std::size_t maxClients = 10000;              // 连接上限：限制同时存在的客户端数量
    std::size_t maxPipelineRequests = 64;        // Pipeline 上限：限制单连接待执行请求数
    std::uint32_t maxFrameBytes = DEFAULT_MAX_FRAME_BYTES; // 帧上限：限制单个协议帧大小
    bool tlsRequired = true;                     // TLS 策略：默认要求加密传输；当前尚无 TLS transport
    bool allowPlaintextForDevelopment = false;    // 明文开发许可：需显式设置
    bool allowInsecureRemote = false;             // 远程明文许可：仅用于隔离测试，默认禁止
};

/*
    类名：NetworkServer
    功能：实现网络服务端内部生命周期编排。
        - 组装 Acceptor、ReactorGroup、连接注册表和命令执行器。
        - 启动和停止监听器及事件循环。
        - 执行服务端排空和关闭顺序。
        - 不对外暴露存储层细节。
    友元类：mydb::net::Controller
*/
class NetworkServer {
private:
    ServerConfig config;                         // 服务配置：监听、TLS、连接和帧限制
    std::shared_ptr<ICommandExecutor> executor;  // 命令执行器：所有结构化请求的业务入口
    std::unique_ptr<ReactorGroup> reactorGroup;  // Reactor 组：管理事件循环线程
    std::unique_ptr<Acceptor> acceptor;          // 接收器：监听并接受新客户端连接
    ServerState state = ServerState::CREATED;    // 服务状态：控制启动、运行、排空和停止流程
    std::atomic<std::size_t> activeClients{0};     // 当前接受并未关闭的连接数
    std::atomic<ConnectionId> nextConnectionId{1}; // 进程内单调递增连接标识

    friend class Controller;                     // Controller：网络服务的对外统一入口

public:
    /*
        函数：NetworkServer
        参数：config：服务端配置；executor：命令执行器
        功能：创建网络服务端并保存运行依赖
        返回：无
    */
    NetworkServer(ServerConfig config, std::shared_ptr<ICommandExecutor> executor);
    ~NetworkServer();

    NetworkServer(const NetworkServer&) = delete;
    NetworkServer& operator=(const NetworkServer&) = delete;

    /*
        函数：start
        参数：无
        功能：启动 Acceptor 和 ReactorGroup
        返回：是否启动成功
    */
    bool start();

    /*
        函数：stop
        参数：graceful：是否优雅停止
        功能：停止接收新连接并按关闭顺序停止网络服务
        返回：无
    */
    void stop(bool graceful);

    /*
        函数：wait
        参数：无
        功能：等待网络服务线程退出
        返回：无
    */
    void wait();

    /*
        函数：getState
        参数：无
        功能：读取网络服务当前状态
        返回：ServerState
    */
    ServerState getState() const;
};

}

#endif
