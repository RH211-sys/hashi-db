#pragma once
#ifndef _MYDB_NET_CONTROLLER_H_
#define _MYDB_NET_CONTROLLER_H_

/*
    模块名：网络服务 Controller
    模块地位：网络服务生命周期对外开放的统一控制入口。
    模块功能描述：网络服务端对外开放的统一入口，负责配置、启动、停止和阶段测试依赖组装；内部模块不直接对外暴露。
*/

#include "../server/network_server.h"
#include <memory>

class Controller;                                // 存储 Controller：仅用于阶段测试的临时前置声明

namespace mydb::net {

/*
    类名：Controller
    地位：网络层对上层调用者开放的服务控制门面。
    功能：提供网络服务端对外开放的统一控制入口。
        - 保存服务配置并组装内部模块。
        - 绑定命令执行器和服务端生命周期。
        - 为阶段测试提供现有存储 Controller 的临时接入点。
        - 隐藏 Acceptor、Reactor、Connection 等内部实现。
    友元类：无
*/
class Controller {
private:
    ServerConfig config;                         // 服务配置：监听地址、TLS 和资源上限
    std::shared_ptr<ICommandExecutor> executor;  // 命令执行器：网络请求到业务逻辑的桥接
    std::unique_ptr<NetworkServer> server;       // 网络服务：内部生命周期实现

public:
    /*
        函数：Controller
        传参：config：网络服务配置
        功能：创建网络服务 Controller
        返回值：无
    */
    explicit Controller(ServerConfig config = {});

    /*
        函数：bindStorageController
        传参：storageController：现有数据存储层 Controller
        功能：绑定阶段测试用的存储执行器
        返回值：无
        备注：当前暂时连接到存储层，用于阶段测试；正式命令解析/执行层稳定后替换该绑定方式
    */
    void bindStorageController(::Controller& storageController);

    /*
        函数：start
        传参：无
        功能：组装并启动网络服务端
        返回值：是否启动成功
    */
    bool start();

    /*
        函数：stop
        传参：无
        功能：立即停止网络服务并按顺序释放连接、Reactor 和监听资源
        返回值：无
    */
    void stop();

    /*
        函数：wait
        传参：无
        功能：等待服务端线程退出
        返回值：无
    */
    void wait();

    /*
        函数：getState
        传参：无
        功能：读取网络服务状态
        返回值：ServerState
    */
    ServerState getState() const;
};

}

#endif
