#pragma once
#ifndef _MYDB_NET_ACCEPTOR_H_
#define _MYDB_NET_ACCEPTOR_H_

/*
    模块名：连接接收器
    功能描述：监听 TCP 地址、接受新连接并将连接分派到 ReactorGroup，不负责客户端协议处理。
*/

#include "../common/net_types.h"
#include <functional>

namespace mydb::net {

using AcceptedConnection = std::function<void(int nativeHandle, const Endpoint& peer)>; // 新连接回调：交给服务端创建 Transport

/*
    类名：Acceptor
    功能：管理监听端点并接收新的 TCP 连接。
        - 创建和关闭监听 socket。
        - 循环 accept 新连接并执行回调。
        - 不参与协议解析和业务命令执行。
    友元类：NetworkServer
*/
class Acceptor {
private:
    Endpoint endpoint;                            // 监听端点：服务端绑定的地址和端口
    std::uint32_t backlog;                        // 监听队列：内核等待接受的连接数量
    AcceptedConnection onAccepted;                // 接收回调：把新连接交给 ReactorGroup

    friend class NetworkServer;                   // NetworkServer：控制监听器启停

public:
    /*
        函数：Acceptor
        参数：endpoint：监听地址；backlog：监听队列长度；onAccepted：新连接回调
        功能：创建监听接收器配置
        返回：无
    */
    Acceptor(Endpoint endpoint, std::uint32_t backlog, AcceptedConnection onAccepted);

    /*
        函数：start
        参数：无
        功能：创建监听 socket 并开始接受连接
        返回：是否启动成功
    */
    bool start();

    /*
        函数：stop
        参数：无
        功能：停止接受连接并关闭监听资源
        返回：无
    */
    void stop();
};

}

#endif
