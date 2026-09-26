#pragma once
#ifndef _MYDB_NET_NETWORK_SERVER_H_
#define _MYDB_NET_NETWORK_SERVER_H_

/*
    模块名：网络服务端
    模块地位：网络服务对外控制器背后的生命周期实现模块。
    模块功能描述：组装 Acceptor、ReactorGroup 和命令执行器，提供服务生命周期内部实现。
*/

#include "../common/net_types.h"
#include "../command/command_executor.h"
#include "../protocol/protocol.h"
#include "../reactor/reactor_group.h"
#include "acceptor.h"
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

namespace mydb::net {

class TlsServerContext;                             // TLS 上下文类型：由 TLS 传输和网络服务共享的配置对象

/*
    类型名：ServerConfig
    地位：网络服务启动时由上层提供的配置契约。
    功能：保存网络服务端的监听、线程、连接、帧大小和 TLS 配置。
*/
struct ServerConfig {
    Endpoint listenEndpoint{"127.0.0.1", 6380}; // 监听端点：默认只监听本机 6380 端口
    std::size_t reactorCount = 0;                // Reactor 数量：0 表示按机器逻辑核数自动计算
    std::size_t maxClients = 10000;              // 连接上限：限制同时存在的客户端数量
    std::size_t maxPipelineRequests = 64;        // Pipeline 上限：限制单连接待执行请求数
    std::uint32_t maxFrameBytes = DEFAULT_MAX_FRAME_BYTES; // 帧上限：限制单个协议帧大小
    bool tlsRequired = true;                     // TLS 策略：默认要求加密传输
    std::string tlsCertificateFile;               // 证书链：TLS 服务端证书 PEM 文件路径
    std::string tlsPrivateKeyFile;                // 私钥：TLS 服务端未加密 PEM 私钥文件路径
    bool allowPlaintextForDevelopment = false;    // 明文开发许可：需显式设置
    bool allowInsecureRemote = false;             // 远程明文许可：仅用于隔离测试，默认禁止
};

/*
    类名：NetworkServer
    地位：网络服务控制门面背后的内部生命周期编排者。
    功能：实现网络服务端内部生命周期编排。
        - 组装 Acceptor、ReactorGroup、连接注册表和命令执行器。
        - 启动和停止监听器及事件循环。
        - 执行服务端立即关闭顺序。
        - 不对外暴露存储层细节。
    友元类：mydb::net::Controller
*/
class NetworkServer {
private:
    ServerConfig config;                         // 服务配置：监听、TLS、连接和帧限制
    std::shared_ptr<ICommandExecutor> executor;  // 命令执行器：所有结构化请求的业务入口
    std::shared_ptr<TlsServerContext> tlsContext; // TLS 上下文：由服务和活动 TLS 连接共享
    std::unique_ptr<ReactorGroup> reactorGroup;  // Reactor 组：管理事件循环线程
    std::unique_ptr<Acceptor> acceptor;          // 接收器：监听并接受新客户端连接
    ServerState state = ServerState::CREATED;    // 服务状态：控制启动、运行和停止流程
    std::atomic<std::size_t> activeClients{0};     // 当前接受并未关闭的连接数
    std::atomic<ConnectionId> nextConnectionId{1}; // 进程内单调递增连接标识

    friend class Controller;                     // Controller：网络服务的对外统一入口

public:
    /*
        函数：NetworkServer
        传参：config：服务端配置；executor：命令执行器
        功能：创建网络服务端并保存运行依赖
        返回值：无
    */
    NetworkServer(ServerConfig config, std::shared_ptr<ICommandExecutor> executor);

    /*
        函数：~NetworkServer
        传参：无
        功能：停止服务并释放服务端持有的资源
        返回值：无
    */
    ~NetworkServer();

    /*
        函数：NetworkServer 复制构造
        传参：源对象：待复制的网络服务器
        功能：禁止复制监听器、Reactor 组和运行状态
        返回值：无
    */
    NetworkServer(const NetworkServer&) = delete;
    /*
        函数：NetworkServer 复制赋值
        传参：源对象：待复制赋值的网络服务器
        功能：禁止替换监听器、Reactor 组和运行状态
        返回值：赋值目标引用类型；该函数已删除，不可调用
    */
    NetworkServer& operator=(const NetworkServer&) = delete;

    /*
        函数：start
        传参：无
        功能：启动 Acceptor 和 ReactorGroup
        返回值：是否启动成功
    */
    bool start();

    /*
        函数：stop
        传参：无
        功能：停止接收新连接并立即关闭网络服务
        返回值：无
    */
    void stop();

    /*
        函数：wait
        传参：无
        功能：等待网络服务线程退出；线程通常已由 stop() 同步回收
        返回值：无
    */
    void wait();

    /*
        函数：getState
        传参：无
        功能：读取网络服务当前状态
        返回值：ServerState
    */
    ServerState getState() const;
};

}

#endif
