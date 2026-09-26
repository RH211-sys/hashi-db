#pragma once
#ifndef _MYDB_NET_SESSION_H_
#define _MYDB_NET_SESSION_H_

/*
    模块名：连接会话
    模块地位：Connection 持有的协议协商与认证会话状态模块。
    模块功能描述：保存协议协商、认证主体和权限等连接级状态，不保存业务实体。
*/

#include "../common/net_types.h"
#include "../protocol/protocol.h"
#include <cstdint>
#include <string>

namespace mydb::net {

/*
    类名：Session
    地位：Connection 持有的单连接协议协商与认证状态。
    功能：保存一个连接的协议协商、认证和授权状态。
        - 记录协商成功的协议版本和特性。
        - 保存认证主体。
        - 为命令解析后的请求提供权限判断。
        - 不保存缓存实体或持久化数据。
    友元类：Connection
*/
class Session {
private:
    std::uint16_t protocolVersion = 0;           // 协议版本：HELLO 协商后的版本号
    bool authenticated = false;                  // 认证状态：是否已完成 AUTH
    std::string principal;                       // 认证主体：用户名或本地管理身份
    std::uint64_t featureBits = 0;               // 特性位：HELLO 协商后的能力集合
    bool negotiated = false;                     // 协商状态：是否已成功完成 HELLO

public:
    /*
        函数：negotiate
        传参：version：客户端协议版本；featureBits：客户端特性位
        功能：协商协议版本和特性
        返回值：是否协商成功
    */
    bool negotiate(std::uint16_t version, std::uint64_t featureBits);

    /*
        函数：authenticate
        传参：principal：认证成功后的主体名称
        功能：将会话标记为已认证并保存主体
        返回值：无
    */
    void authenticate(std::string principal);

    /*
        函数：canExecute
        传参：opcode：待执行命令；source：请求来源标识
        功能：根据会话认证状态和权限判断命令是否允许执行
        返回值：是否允许
        备注：具体 ACL 数据来源由管理/认证模块提供
    */
    bool canExecute(Opcode opcode, RequestSource source) const;

    /*
        函数：isAuthenticated
        传参：无
        功能：读取认证状态
        返回值：是否已认证
    */
    bool isAuthenticated() const;

    /*
        函数：isNegotiated
        传参：无
        功能：读取协议握手状态
        返回值：是否已完成 HELLO
    */
    bool isNegotiated() const;

    /*
        函数：getPrincipal
        传参：无
        功能：读取认证主体
        返回值：主体名称
    */
    const std::string& getPrincipal() const;

    /*
        函数：getFeatureBits
        传参：无
        功能：读取协商成功的特性位
        返回值：特性位掩码
    */
    std::uint64_t getFeatureBits() const;
};

}

#endif
