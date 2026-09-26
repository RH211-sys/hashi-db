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
    地位：单个客户端连接的状态与请求顺序所有者。
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
    /*
        地位：等待前序请求处理完成后生成的协议错误记录。
        功能：保存错误响应所需的请求标识、命令码、状态和说明。
    */
    struct PendingProtocolError {
        RequestId requestId;                       // 请求标识：对应无法解析或校验的帧
        Opcode opcode;                              // 命令码：用于构造关联错误响应
        ErrorCode error;                            // 错误类型：协议层稳定错误分类
        std::string message;                        // 错误说明：有界输出给客户端
    };

    /*
        地位：Connection 内部的传输操作等待状态。
        功能：记录 TLS 非阻塞交叉读写需要重试的操作方向。
    */
    enum class PendingTransportOperation : std::uint8_t {
        NONE,                                       // 无挂起操作：当前不需要等待交叉 I/O 重试
        READ,                                       // 读取挂起：等待 TLS 读取所需的事件就绪
        WRITE                                       // 写入挂起：等待 TLS 写入所需的事件就绪
    };

    ConnectionId id;                                   // 连接标识：进程内唯一
    ReactorId ownerReactor;                            // 所属 Reactor：连接线程亲和性的依据
    ConnectionState state;                             // 生命周期状态：控制协议阶段、传输和关闭
    ConnectionActivityState activityState = ConnectionActivityState::IDLE; // 请求状态：控制派发和等待队列
    std::uint64_t generation = 0;                      // 连接代际：区分复用连接标识的过期完成结果
    Endpoint peer;                                     // 对端地址：随请求交给业务层
    Session session;                                   // 会话状态：协议协商与认证结果
    std::unique_ptr<Transport> transport;              // 传输对象：负责非阻塞读写和关闭
    FrameCodec codec;                                  // 帧编解码器：拆包粘包与响应编码
    CommandParser parser;                              // 命令解析器：帧到结构化命令的校验
    std::deque<CommandRequest> pipeline;               // 待执行队列：已完整解析但尚未提交的请求
    std::optional<PendingProtocolError> pendingProtocolError; // 延迟错误：排在已接收请求之后处理
    ByteBuffer pendingInput;                           // 输入缓冲：尚未组成完整帧的字节
    std::size_t pendingInputOffset = 0;                // 输入消费位置：当前字节块已解析到的下标
    std::deque<ByteBuffer> outputQueue;                // 输出队列：待发送的完整响应帧
    std::deque<std::size_t> outputOffsets;             // 输出进度：与输出队列一一对应的已发送字节数
    Opcode activeOpcode = Opcode::PING;                // 在途命令：请求移动到执行器后保留
    std::optional<RequestId> activeRequestId;          // 在途请求标识：关联异步完成响应
    std::optional<std::uint64_t> pendingFeatureBits;   // HELLO 能力位：请求移动后保留
    std::string pendingPrincipal;                      // AUTH 主体：请求移动后保留
    bool pendingAuthentication = false;                // AUTH 状态：标记是否等待认证结果
    ICommandExecutor* executorForResume = nullptr;     // 执行器引用：完成回投后继续调度使用
    std::function<void()> releaseSlot;                 // 槽位释放回调：连接关闭时调用一次
    ConnectionCompletionPoster completionPoster;       // 完成投递器：回投到所属 Reactor
    std::size_t queuedOutputBytes = 0;                 // 输出积压字节数：限制单连接输出内存
    std::size_t maxPipelineRequests;                   // 流水线上限：待执行队列和在途请求的总深度上限
    std::size_t maxFrameBytes;                         // 帧上限：单帧最大字节数
    std::size_t maxBufferedInputBytes;                 // 输入上限：单连接未解析输入的最大字节数
    bool closeAfterOutput = false;                     // 写完即关：输出清空后关闭连接
    bool pollerRegistered = false;                     // 轮询注册状态：当前是否加入 Reactor 事件轮询
    bool readPaused = false;                           // 读取暂停：队列或缓冲达到上限时停止关注可读
    bool peerClosed = false;                           // 对端关闭发送方向：排空已接收请求和响应后关闭
    PendingTransportOperation pendingTransportOperation = PendingTransportOperation::NONE; // 挂起操作：等待 OpenSSL 所需的交叉 I/O 就绪
    std::size_t pendingTransportBytes = 0;              // 挂起长度：重试时保持相同的缓冲区长度

    friend class Reactor;
    friend class ReactorGroup;
    friend class ConnectionRegistry;

    /*
        函数：dispatchNext
        传参：executor：处理结构化命令的执行器
        功能：按连接顺序校验并派发队首请求
        返回值：无
    */
    void dispatchNext(ICommandExecutor& executor);

    /*
        函数：processInput
        传参：executor：处理结构化命令的执行器
        功能：增量解析暂存输入并处理其中的完整帧
        返回值：输入处理期间连接是否仍可用
    */
    bool processInput(ICommandExecutor& executor);

    /*
        函数：handleFrame
        传参：frame：完整协议帧；executor：处理结构化命令的执行器
        功能：校验并解析帧，将请求纳入当前连接的处理顺序
        返回值：帧处理期间连接是否仍可用
    */
    bool handleFrame(Frame frame, ICommandExecutor& executor);

    /*
        函数：queueProtocolError
        传参：requestId：出错请求标识；opcode：命令码；error：错误分类；message：错误说明
        功能：生成关联协议错误响应并安排连接关闭
        返回值：无
    */
    void queueProtocolError(RequestId requestId, Opcode opcode, ErrorCode error, std::string message);

    /*
        函数：retryPendingRead
        传参：bytesRead：输出本次读取的字节数
        功能：按传输层要求重试尚未完成的读取操作
        返回值：传输操作结果
    */
    TransportResult retryPendingRead(std::size_t& bytesRead);

    /*
        函数：retryPendingWrite
        传参：bytesWritten：输出本次写出的字节数
        功能：按传输层要求重试尚未完成的写入操作
        返回值：传输操作结果
    */
    TransportResult retryPendingWrite(std::size_t& bytesWritten);

    /*
        函数：interestEvents
        传参：无
        功能：汇总当前连接需要监听的传输事件
        返回值：Poller 事件位掩码
    */
    std::uint32_t interestEvents() const;

public:
    /*
        函数：Connection
        传参：id：连接标识；ownerReactor：所属 Reactor；transport：连接传输；maxPipelineRequests：流水线上限；peer：对端地址；maxFrameBytes：帧上限；generation：连接代际；releaseSlot：连接槽位释放回调；completionPoster：完成结果投递回调
        功能：创建连接并接管传输和连接生命周期依赖
        返回值：无
    */
    Connection(ConnectionId id, ReactorId ownerReactor, std::unique_ptr<Transport> transport,
               std::size_t maxPipelineRequests, Endpoint peer = {},
               std::uint32_t maxFrameBytes = DEFAULT_MAX_FRAME_BYTES,
               std::uint64_t generation = 0, std::function<void()> releaseSlot = {},
                ConnectionCompletionPoster completionPoster = {});

    /*
        函数：~Connection
        传参：无
        功能：关闭连接并释放其传输及缓冲资源
        返回值：无
    */
    ~Connection();

    /*
        函数：Connection 复制构造
        传参：源对象：待复制的连接状态
        功能：禁止复制连接缓冲、请求队列和传输所有权
        返回值：无
    */
    Connection(const Connection&) = delete;
    /*
        函数：Connection 复制赋值
        传参：源对象：待复制赋值的连接状态
        功能：禁止替换 Reactor 线程独占的连接状态
        返回值：赋值目标引用类型；该函数已删除，不可调用
    */
    Connection& operator=(const Connection&) = delete;

    /*
        函数：onReadable
        传参：executor：命令执行器；readBudget：本次读取预算
        功能：读取并解析连接输入，按流水线上限暂存请求
        返回值：无
    */
    void onReadable(ICommandExecutor& executor, std::size_t readBudget);

    /*
        函数：onPeerHalfClose
        传参：无
        功能：记录对端半关闭并安排已接收请求和响应的收尾
        返回值：无
    */
    void onPeerHalfClose();

    /*
        函数：onWritable
        传参：writeBudget：本次写出预算
        功能：按顺序写出已编码响应并更新连接状态
        返回值：无
    */
    void onWritable(std::size_t writeBudget);

    /*
        函数：complete
        传参：response：执行层返回的结构化结果
        功能：关联当前在途请求并生成有序网络响应
        返回值：无
    */
    void complete(CommandResponse response);

    /*
        函数：close
        传参：无
        功能：关闭连接并清理该连接持有的请求与传输资源
        返回值：无
    */
    void close();

    /*
        函数：getId
        传参：无
        功能：读取连接标识
        返回值：当前连接标识
    */
    ConnectionId getId() const;

    /*
        函数：getState
        传参：无
        功能：读取连接生命周期状态
        返回值：当前连接状态
    */
    ConnectionState getState() const;
};

}

#endif
