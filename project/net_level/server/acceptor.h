#pragma once
#ifndef _MYDB_NET_ACCEPTOR_H_
#define _MYDB_NET_ACCEPTOR_H_

/*
    模块名：连接接收器
    模块地位：NetworkServer 与 ReactorGroup 之间的新连接接入模块。
    模块功能描述：监听 TCP 地址、接受新连接并将连接分派到 ReactorGroup，不负责客户端协议处理。
*/

#include "../common/net_types.h"
#include "../transport/transport.h"
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace mydb::net {

using ClientSlotRelease = std::function<void()>; // 槽位释放回调：归还客户端连接槽位，至多调用一次
using AcceptedConnection = std::function<void(std::unique_ptr<Transport> transport, const Endpoint& peer,
                                               ClientSlotRelease releaseSlot)>; // 接收回调：交接传输、对端地址和槽位释放动作

/*
    类名：Acceptor
    地位：NetworkServer 与新建客户端 socket 之间的监听接入模块。
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
    std::size_t maxClients = 10000;                // 连接硬上限
    std::atomic<std::size_t>* activeClients = nullptr; // 服务端连接计数，由接收器线程安全更新
    std::intptr_t listenHandle = -1;              // 监听 socket：平台句柄以整数形式保存
    std::atomic<bool> running{false};              // 接收状态：控制 accept 线程退出
    std::thread worker;                            // 接收线程：非阻塞 accept 循环

    friend class NetworkServer;                   // NetworkServer：控制监听器启停

public:
    /*
        函数：Acceptor
        传参：endpoint：监听地址；backlog：监听队列长度；onAccepted：新连接回调
        功能：创建监听接收器配置
        返回值：无
    */
    Acceptor(Endpoint endpoint, std::uint32_t backlog, AcceptedConnection onAccepted);

    /*
        函数：start
        传参：无
        功能：创建监听 socket 并开始接受连接
        返回值：是否启动成功
    */
    bool start();

    /*
        函数：stop
        传参：无
        功能：停止接受连接并关闭监听资源
        返回值：无
    */
    void stop();
};

}

#endif
