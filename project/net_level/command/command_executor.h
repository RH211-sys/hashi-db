#pragma once
#ifndef _MYDB_NET_COMMAND_EXECUTOR_H_
#define _MYDB_NET_COMMAND_EXECUTOR_H_

/*
    模块名：命令执行模块
    功能描述：定义网络命令到业务执行层的稳定接口；网络层只提交结构化请求，不直接依赖 Cache/Disk 私有实现。
*/

#include "../protocol/protocol.h"
#include <functional>

class Controller;                                // 存储 Controller：阶段测试适配器依赖的现有存储层入口

namespace mydb::net {

using CommandCompletion = std::function<void(CommandResponse)>; // 完成回调：命令执行结束后投递结构化响应

/*
    类名：ICommandExecutor
    功能：定义网络层向业务执行层提交命令的异步接口。
        - 接收已经完成协议解析的 CommandRequest。
        - 通过 completion 返回结构化响应。
        - 不直接操作 Connection，避免业务线程触碰网络对象。
    友元类：无
*/
class ICommandExecutor {
public:
    /*
        函数：~ICommandExecutor
        参数：无
        功能：销毁命令执行器接口
        返回：无
    */
    virtual ~ICommandExecutor() = default;

    /*
        函数：execute
        参数：request：结构化命令；completion：异步完成回调
        功能：提交命令执行，保证 completion 最终调用一次
        返回：无
        备注：执行器不得直接操作 Connection；完成结果由所属 Reactor 接管
    */
    virtual void execute(CommandRequest request, CommandCompletion completion) = 0;
};

/*
    类名：StorageControllerExecutor
    功能：将网络命令临时适配到现有存储层 Controller。
        - 用于网络层阶段测试和 CRUD 闭环验证。
        - 后续由正式命令路由和业务执行层替换。
        - 不代表最终的存储层对接方式。
    友元类：无
*/
class StorageControllerExecutor final : public ICommandExecutor {
private:
    ::Controller& storageController;             // 存储 Controller：阶段测试期间承接网络命令

public:
    /*
        函数：StorageControllerExecutor
        参数：storageController：现有存储层 Controller
        功能：建立网络命令到存储 Controller 的阶段测试适配
        返回：无
        备注：当前暂时连接到存储层，用于阶段测试；正式版本由独立命令执行层替换
    */
    explicit StorageControllerExecutor(::Controller& storageController);

    /*
        函数：execute
        参数：request：结构化网络命令；completion：异步结果回调
        功能：将首版命令映射到现有存储 Controller 的测试接口
        返回：无
        备注：这是临时桥接，不代表最终业务命令架构
    */
    void execute(CommandRequest request, CommandCompletion completion) override;
};

}

#endif
