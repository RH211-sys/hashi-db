#pragma once
#ifndef _MYDB_NET_CONNECTION_REGISTRY_H_
#define _MYDB_NET_CONNECTION_REGISTRY_H_

/*
    模块名：连接注册表
    模块地位：定义跨 Reactor 连接查询与关闭管理的接口边界。
    模块功能描述：维护连接 ID 到连接快照/关闭任务的跨 Reactor 管理边界，不直接暴露 socket fd。
*/

#include "../common/net_types.h"
#include <vector>

namespace mydb::net {

/*
    类型名：ConnectionSnapshot
    地位：ConnectionRegistry 对外提供的连接状态只读结果。
    功能：表示某个连接在指定时刻的只读管理快照。
*/
struct ConnectionSnapshot {
    ConnectionId id = 0;                         // 连接 ID：快照对应的客户端连接
    ReactorId reactor = 0;                       // Reactor ID：连接所属事件循环
    ConnectionState state = ConnectionState::CLOSED; // 状态：快照生成时的连接状态
    Endpoint peer;                                // 对端端点：客户端地址和端口
};

/*
    类名：ConnectionRegistry
    地位：NetworkServer 与各 Reactor 之间的跨连接管理边界。
    功能：提供跨 Reactor 的连接查询和关闭管理边界。
        - 保存连接 ID 到所属 Reactor 的映射。
        - 生成只读连接快照。
        - 通过 Reactor 投递关闭任务，不直接操作 socket。
    友元类：NetworkServer
*/
class ConnectionRegistry {
private:
    friend class NetworkServer;                  // NetworkServer：组装并持有注册表实现

public:
    /*
        函数：~ConnectionRegistry
        传参：无
        功能：销毁连接注册表
        返回值：无
    */
    virtual ~ConnectionRegistry() = default;

    /*
        函数：list
        传参：无
        功能：读取当前连接的只读快照
        返回值：连接快照列表
    */
    virtual std::vector<ConnectionSnapshot> list() const = 0;

    /*
        函数：kill
        传参：connectionId：待关闭连接标识
        功能：向连接所属 Reactor 投递立即关闭任务
        返回值：是否成功找到并投递
    */
    virtual bool kill(ConnectionId connectionId) = 0;
};

}

#endif
