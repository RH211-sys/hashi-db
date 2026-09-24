#pragma once
#ifndef _MYDB_NET_CONNECTION_H_
#define _MYDB_NET_CONNECTION_H_

/*
    模块名：客户端连接
    功能描述：维护单个客户端的传输状态、会话状态、输入输出缓冲、命令流水线和连接级背压。
*/

#include "../command/command_parser.h"
#include "../command/command_executor.h"
#include "../protocol/frame_codec.h"
#include "session.h"
#include "../transport/transport.h"
#include <cstddef>
#include <deque>
#include <memory>

namespace mydb::net {

/*
    类名：Connection
    功能：维护一个客户端连接的完整网络状态。
        - 保存 Transport、Session、FrameCodec 和命令流水线。
        - 在所属 Reactor 中处理读写事件。
        - 管理输出队列、异步完成回投和连接级背压。
        - 连接 ID 与 generation 一起用于防止 fd 复用导致的错误回调。
    友元类：Reactor、ReactorGroup、ConnectionRegistry
*/
class Connection {
private:
    ConnectionId id;                             // 连接 ID：进程内稳定的连接身份
    ReactorId ownerReactor;                      // 所属 Reactor：唯一允许操作本连接 socket 的事件循环
    ConnectionState state;                       // 连接状态：协议握手、就绪或关闭阶段
    std::uint64_t generation = 0;                // 代际：防止旧连接异步结果写入 fd 复用后的新连接
    Endpoint peer;                               // 对端端点：客户端地址和端口
    Session session;                             // 会话：协议版本、认证主体和权限状态
    std::unique_ptr<Transport> transport;        // 传输层：plain TCP 或 TLS 的非阻塞实现
    FrameCodec codec;                            // 帧编解码器：处理 TCP 拆包、粘包和协议帧
    CommandParser parser;                        // 命令解析器：将 Frame 转换为 CommandRequest
    std::deque<CommandRequest> pipeline;         // 请求流水线：有界保存已解析但尚未执行的请求
    std::deque<ByteBuffer> outputQueue;          // 输出队列：保存等待写出的响应字节
    std::size_t maxPipelineRequests;             // 流水线上限：限制单连接排队请求数量

    friend class Reactor;                        // Reactor：驱动连接的读写和生命周期
    friend class ReactorGroup;                   // ReactorGroup：创建并分派连接
    friend class ConnectionRegistry;             // ConnectionRegistry：读取连接快照并投递关闭任务

public:
    /*
        函数：Connection
        参数：id：连接标识；ownerReactor：所属 Reactor；transport：非阻塞传输对象；maxPipelineRequests：流水线请求上限
        功能：创建一个归属于指定 Reactor 的客户端连接
        返回：无
    */
    Connection(ConnectionId id, ReactorId ownerReactor, std::unique_ptr<Transport> transport,
               std::size_t maxPipelineRequests);

    /*
        函数：onReadable
        参数：executor：命令执行器；readBudget：单轮最大读取字节数
        功能：读取字节、解析完整帧、执行会话校验并将命令放入有界流水线
        返回：无
    */
    void onReadable(ICommandExecutor& executor, std::size_t readBudget);

    /*
        函数：onWritable
        参数：writeBudget：单轮最大写出字节数
        功能：按顺序写出已经完成的响应
        返回：无
    */
    void onWritable(std::size_t writeBudget);

    /*
        函数：complete
        参数：response：命令完成响应
        功能：接收 Reactor 回投的异步结果并加入连接输出队列
        返回：无
        备注：必须校验 connectionId、requestId 和连接代际，不能只依赖 fd
    */
    void complete(CommandResponse response);

    /*
        函数：close
        参数：graceful：是否尝试发送最后响应后关闭
        功能：进入关闭状态并释放连接资源
        返回：无
    */
    void close(bool graceful);

    /*
        函数：getId
        参数：无
        功能：读取连接标识
        返回：ConnectionId
    */
    ConnectionId getId() const;

    /*
        函数：getState
        参数：无
        功能：读取当前连接状态
        返回：ConnectionState
    */
    ConnectionState getState() const;
};

}

#endif
