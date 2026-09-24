#pragma once
#ifndef _MYDB_NET_CONNECTION_H_
#define _MYDB_NET_CONNECTION_H_

/*
    模块名：客户端连接
    模块地位：Reactor 与协议编解码、命令执行之间的单连接状态持有者
    模块功能描述：维护单个客户端的传输状态、会话状态、输入输出缓冲和命令流水线，保证同连接请求有序执行。
*/

#include "../command/command_parser.h"
#include "../command/command_executor.h"
#include "../protocol/frame_codec.h"
#include "session.h"
#include "../transport/transport.h"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace mydb::net {

using ConnectionCompletionPoster = std::function<void(CommandResponse)>; // 完成投递器：把响应交回所属 Reactor

/*
    类名：Connection
    功能：管理单个客户端连接。
        - 非阻塞读取并按帧解析输入，最多在途一个业务请求。
        - 把结构化响应编码为输出缓冲，按预算写出。
        - 维护 HELLO/AUTH 会话阶段，并在未授权时拒绝业务命令。
        - 不直接访问存储层，只通过 ICommandExecutor 提交命令。
    友元类：Reactor、ReactorGroup、ConnectionRegistry
    说明：本类所有状态只能在其所属 Reactor 线程内访问。
*/
class Connection {
private:
    ConnectionId id;                                   // 连接标识：进程内唯一
    ReactorId ownerReactor;                            // 所属 Reactor：连接线程亲和性的依据
    ConnectionState state;                             // 连接状态：控制读取、调度和关闭
    std::uint64_t generation = 0;                      // 连接代际：区分复用连接标识的过期完成结果
    Endpoint peer;                                     // 对端地址：随请求交给业务层
    Session session;                                   // 会话状态：协议协商与认证结果
    std::unique_ptr<Transport> transport;              // 传输对象：负责非阻塞读写和关闭
    FrameCodec codec;                                  // 帧编解码器：拆包粘包与响应编码
    CommandParser parser;                              // 命令解析器：帧到结构化命令的校验
    std::deque<CommandRequest> pipeline;               // 待执行队列：已完整解析但尚未提交的请求
    ByteBuffer pendingInput;                           // 输入缓冲：尚未组成完整帧的字节
    std::size_t pendingInputOffset = 0;                // 输入消费位置：当前字节块已解析到的下标
    std::deque<ByteBuffer> outputQueue;                // 输出队列：待发送的完整响应帧
    std::deque<std::size_t> outputOffsets;             // 输出进度：与输出队列一一对应的已发送字节数
    std::optional<CommandRequest> activeRequest;       // 在途请求：同一时刻最多一个
    ICommandExecutor* executorForResume = nullptr;     // 执行器引用：完成回投后继续调度使用
    std::function<void()> releaseSlot;                 // 槽位释放回调：连接关闭时调用一次
    ConnectionCompletionPoster completionPoster;       // 完成投递器：回投到所属 Reactor
    std::size_t queuedOutputBytes = 0;                 // 输出积压字节数：限制单连接输出内存
    std::size_t maxPipelineRequests;                   // 流水线上限：待执行队列和在途请求的总深度上限
    std::size_t maxFrameBytes;                         // 帧上限：单帧最大字节数
    std::size_t maxBufferedInputBytes;                 // 输入上限：单连接未解析输入的最大字节数
    bool closeAfterOutput = false;                     // 写完即关：输出清空后关闭连接
    bool readPaused = false;                           // 读取暂停：队列或缓冲达到上限时停止关注可读
    bool draining = false;                             // 排空中：停止接收新请求，继续完成已接收请求
    bool peerClosed = false;                           // 对端已关闭：只允许写完剩余响应

    friend class Reactor;
    friend class ReactorGroup;
    friend class ConnectionRegistry;

    void dispatchNext(ICommandExecutor& executor);
    bool processInput(ICommandExecutor& executor);
    bool handleFrame(Frame frame, ICommandExecutor& executor);
    void queueProtocolError(RequestId requestId, Opcode opcode, ErrorCode error, std::string message);
    void beginDrain(ICommandExecutor& executor);
    std::uint32_t interestEvents() const;

public:
    Connection(ConnectionId id, ReactorId ownerReactor, std::unique_ptr<Transport> transport,
               std::size_t maxPipelineRequests, Endpoint peer = {},
               std::uint32_t maxFrameBytes = DEFAULT_MAX_FRAME_BYTES,
               std::uint64_t generation = 0, std::function<void()> releaseSlot = {},
               ConnectionCompletionPoster completionPoster = {});
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void onReadable(ICommandExecutor& executor, std::size_t readBudget);
    void onWritable(std::size_t writeBudget);
    void complete(CommandResponse response);
    void close(bool graceful);
    ConnectionId getId() const;
    ConnectionState getState() const;
};

}

#endif
