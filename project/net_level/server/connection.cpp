/*
    模块名：客户端连接
    模块地位：Reactor 与协议编解码、命令执行之间的单连接状态持有者
    模块功能描述：处理单个连接的非阻塞读写、协议解析和有序异步请求调度，所有状态只在所属 Reactor 线程访问。
*/

#include "connection.h"
#include "../command/protocol_fields.h"
#include "../reactor/poller.h"
#include "../common/utf8.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>

namespace mydb::net {
namespace {

constexpr std::size_t MAX_OUTPUT_BYTES = 16U * 1024U * 1024U; // 输出积压上限：超过即视为不可恢复并关闭连接
constexpr std::size_t MAX_ERROR_MESSAGE_BYTES = 256;          // 错误信息上限：避免把超长文本写回客户端
constexpr std::size_t READ_BUDGET_EXTRA_BYTES = 64U * 1024U;  // 输入缓冲余量：在单帧上限之上允许的未解析字节

std::uint64_t readU64(const ByteBuffer& value) {
    std::uint64_t result = 0; // 解码结果：按大端顺序累积得到的 64 位数值
    for (std::uint8_t byte : value) { // 字节：当前参与大端数值累积的字段内容
        result = (result << 8U) | byte;
    }
    return result;
}

/*
    函数：boundedUtf8
    传参：message：原始信息
    功能：把信息截断到协议上限，并保证截断后仍是合法 UTF-8
    返回值：可直接写入响应帧的文本
*/
std::string boundedUtf8(const std::string& message) {
    const std::size_t length = std::min(message.size(), MAX_ERROR_MESSAGE_BYTES); // 截断长度：限制错误文本最大字节数
    std::size_t validLength = length; // 有效前缀长度：最终保留的完整 UTF-8 字节数
    while (validLength > 0 && !isValidUtf8(reinterpret_cast<const std::uint8_t*>(message.data()), validLength)) {
        --validLength;
    }
    return message.substr(0, validLength);
}

/*
    函数：hasReservedResponseFields
    传参：fields：业务返回的结果字段
    功能：判断结果字段是否占用了协议保留字段
    返回值：是否包含保留字段
    备注：状态码和错误信息由网络层统一填充，业务不得自行占用
*/
bool hasReservedResponseFields(const std::vector<TlvField>& fields) {
    return std::any_of(fields.begin(), fields.end(), [](const TlvField& field) { // field：当前用于判断是否为保留响应字段的字段
        return field.fieldId == fieldId(FieldId::STATUS_CODE) || field.fieldId == fieldId(FieldId::MESSAGE);
    });
}

const TlvField* findField(const CommandRequest& request, FieldId id) {
    const auto expected = fieldId(id); // 目标编号：待查找协议字段的数值标识
    const auto found = std::find_if(request.fields.begin(), request.fields.end(), [expected](const TlvField& field) { // field：当前比较字段；匹配结果：指向编号相同的请求字段
        return field.fieldId == expected;
    });
    return found == request.fields.end() ? nullptr : &*found;
}

/*
    函数：appendStatus
    传参：frame：待补充的响应帧；code：网络层状态码
    功能：把一个状态码字段追加到响应帧
    返回值：无
*/
void appendStatus(Frame& frame, ErrorCode code) {
    TlvField status; // 状态字段：编码响应的稳定错误分类
    status.fieldId = fieldId(FieldId::STATUS_CODE);
    status.type = FieldType::U64;
    const auto value = static_cast<std::uint64_t>(code); // 状态数值：转换为协议要求的 64 位字段值
    for (int shift = 56; shift >= 0; shift -= 8) { // 位移量：按大端顺序提取状态字段各字节
        status.value.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
    frame.fields.push_back(std::move(status));
}

}

Connection::Connection(ConnectionId id, ReactorId ownerReactor, std::unique_ptr<Transport> transport,
                       std::size_t maxPipelineRequests, Endpoint peer, std::uint32_t maxFrameBytes,
                       std::uint64_t generation, std::function<void()> releaseSlot,
                       ConnectionCompletionPoster completionPoster)
    : id(id), ownerReactor(ownerReactor), state(ConnectionState::ACCEPTED), generation(generation),
      peer(std::move(peer)), transport(std::move(transport)), codec(maxFrameBytes),
      releaseSlot(std::move(releaseSlot)), completionPoster(std::move(completionPoster)),
      maxPipelineRequests(maxPipelineRequests), maxFrameBytes(maxFrameBytes),
      maxBufferedInputBytes(static_cast<std::size_t>(maxFrameBytes) <=
              std::numeric_limits<std::size_t>::max() - READ_BUDGET_EXTRA_BYTES
          ? static_cast<std::size_t>(maxFrameBytes) + READ_BUDGET_EXTRA_BYTES
          : std::numeric_limits<std::size_t>::max()) {
    if (maxPipelineRequests == 0 || maxFrameBytes < BASE_HEADER_LENGTH) {
        // 配置无法承载有效请求或帧头，关闭传输并将连接置为断开状态。
        this->transport->close();
        state = ConnectionState::CLOSED;
        activityState = ConnectionActivityState::DISCONNECTED;
        if (this->releaseSlot) {
            // 存在已接管的连接槽位回调，立即释放该槽位。
            auto release = std::move(this->releaseSlot); // 释放回调：取出已接管的连接槽位释放动作
            release();
        }
        return;
    }
    const TransportResult result = this->transport->handshake(); // 握手结果：初始化连接传输阶段的状态
    if (result == TransportResult::OK) {
        // 传输层已可用，进入等待 HELLO 协商阶段。
        state = ConnectionState::WAIT_HELLO;
    } else if (result == TransportResult::WOULD_BLOCK) {
        // TLS 握手需要后续 I/O 就绪，保留连接并等待 Reactor 推进。
        state = ConnectionState::TLS_HANDSHAKE;
    } else {
        // 握手发生不可恢复错误，关闭传输并释放连接槽位。
        state = ConnectionState::CLOSED;
        activityState = ConnectionActivityState::DISCONNECTED;
        this->transport->close();
        if (this->releaseSlot) {
            // 存在已接管的连接槽位回调，释放对应容量。
            auto release = std::move(this->releaseSlot); // 释放回调：取出已接管的连接槽位释放动作
            release();
        }
    }
}

Connection::~Connection() {
    close();
}

/*
    函数：retryPendingRead
    功能：使用原有长度重试等待写就绪的 TLS 读取
    传参：bytesRead：本次读取的明文字节数输出
    返回值：传输操作结果
*/
TransportResult Connection::retryPendingRead(std::size_t& bytesRead) {
    const std::size_t originalSize = pendingInput.size(); // 原始长度：读取失败或等待时恢复输入缓冲边界
    const TransportResult result = transport->read(pendingInput, pendingTransportBytes); // 读取结果：说明传输是否完成或需要重试
    bytesRead = pendingInput.size() - originalSize;
    if (result != TransportResult::WOULD_BLOCK) {
        // 读取操作不再等待重试，清除挂起操作和原缓冲长度记录。
        pendingTransportOperation = PendingTransportOperation::NONE;
        pendingTransportBytes = 0;
    }
    return result;
}

/*
    函数：retryPendingWrite
    功能：使用原缓冲区和原长度重试等待读就绪的 TLS 写入
    传参：bytesWritten：本次写出的字节数输出
    返回值：传输操作结果
*/
TransportResult Connection::retryPendingWrite(std::size_t& bytesWritten) {
    ByteBuffer& bytes = outputQueue.front(); // 当前帧：输出队列中等待继续发送的响应字节
    std::size_t& offset = outputOffsets.front(); // 写入进度：当前响应帧已发送的字节数
    const std::span<const std::uint8_t> chunk(bytes.data() + offset, pendingTransportBytes); // 重试片段：保持 TLS 重试要求的缓冲地址和长度
    std::size_t chunkOffset = 0; // 分块进度：本次写操作消耗的字节数
    const TransportResult result = transport->write(chunk, chunkOffset); // 写入结果：说明当前传输是否完成或需要重试
    offset += chunkOffset;
    queuedOutputBytes -= chunkOffset;
    bytesWritten = chunkOffset;
    if (offset == bytes.size()) {
        // 当前响应帧已全部发送，移除帧数据和对应写入进度。
        outputQueue.pop_front();
        outputOffsets.pop_front();
    }
    if (result != TransportResult::WOULD_BLOCK) {
        // 写入操作不再等待重试，清除挂起操作和原缓冲长度记录。
        pendingTransportOperation = PendingTransportOperation::NONE;
        pendingTransportBytes = 0;
    }
    return result;
}

/*
    函数：onReadable
    传参：executor：命令执行器；readBudget：本次最多读取的字节数
    功能：推进握手或读取输入，并按帧解析后提交命令
    返回值：无
    备注：readBudget 为 0 时只解析已缓冲数据，不进行读取
*/
void Connection::onReadable(ICommandExecutor& executor, std::size_t readBudget) {
    executorForResume = &executor;
    if (state == ConnectionState::CLOSED) {
        // 连接已经关闭，不再读取或处理该连接的数据。
        return;
    }
    if (state == ConnectionState::TLS_HANDSHAKE) {
        // 连接仍处于 TLS 握手阶段，推进握手状态后结束本次可读处理。
        const TransportResult result = transport->handshake(); // 握手结果：推进当前连接的 TLS 协商
        if (result == TransportResult::OK) {
            // TLS 握手完成，切换到等待 HELLO 协商的阶段。
            state = ConnectionState::WAIT_HELLO;
        } else if (result != TransportResult::WOULD_BLOCK) {
            // TLS 握手遇到不可恢复错误，关闭连接并清理其资源。
            close();
        }
        return;
    }
    if (pendingTransportOperation != PendingTransportOperation::NONE) {
        // 存在挂起传输操作，优先按传输层要求重试，不开始普通读取。
        if ((transport->events() & WANT_READ) == 0) {
            // 传输操作当前不等待可读事件，保留挂起操作并等待对应事件。
            return;
        }
        if (pendingTransportOperation == PendingTransportOperation::WRITE) {
            // 挂起操作为写入，重试尚未完成的输出。
            std::size_t bytesWritten = 0; // 已写字节数：记录重试写操作的进度
            const TransportResult result = retryPendingWrite(bytesWritten); // 重试结果：决定继续等待或收尾
            if (result == TransportResult::WOULD_BLOCK) {
                // 重试写仍需等待传输就绪，保留操作并结束本轮处理。
                return;
            }
            if (result == TransportResult::FAILED || result == TransportResult::PEER_CLOSED) {
                // 重试写失败或对端已关闭，关闭当前连接。
                close();
                return;
            }
            onWritable(0);
            if (state == ConnectionState::CLOSED) {
                // 写入后连接已关闭，不再继续处理可读事件。
                return;
            }
        } else {
            // 挂起的是读操作，重试传输读取并处理新收到的数据。
            std::size_t bytesRead = 0; // 已读字节数：记录重试读操作新增的输入
            const TransportResult result = retryPendingRead(bytesRead); // 重试结果：决定继续等待或处理连接关闭
            readBudget -= std::min(readBudget, bytesRead);
            if (bytesRead != 0 && (state != ConnectionState::CLOSING || peerClosed) &&
                !processInput(executor)) {
                // 新输入解析失败或连接进入关闭流程，结束本轮读取。
                return;
            }
            if (state == ConnectionState::CLOSED) {
                // 输入处理已关闭连接，不再执行后续读操作。
                return;
            }
            if (result == TransportResult::PEER_CLOSED) {
                // 对端关闭写方向，记录半关闭并排空已接收数据。
                onPeerHalfClose();
                return;
            }
            if (result == TransportResult::FAILED) {
                // 传输读取发生不可恢复错误，关闭当前连接。
                close();
                return;
            }
            if (result == TransportResult::WOULD_BLOCK) {
                // 重试读仍需等待传输就绪，保留操作并结束本轮处理。
                return;
            }
            if (state == ConnectionState::CLOSING) {
                // 重试读后连接仍在关闭阶段，停止本轮读取并按状态清理输入。
                if (activityState == ConnectionActivityState::DISCONNECTED) {
                    // 连接已断开，清除不再处理的输入缓存。
                    pendingInput.clear();
                    pendingInputOffset = 0;
                }
                // 连接正在关闭，停止继续读取输入。
                return;
            }
        }
    }
    if (state == ConnectionState::CLOSING) {
        // 连接处于关闭阶段，不再接收新的输入。
        return;
    }
    if (!processInput(executor)) {
        // 已缓存输入无法继续处理，等待后续状态推进。
        return;
    }
    if (state == ConnectionState::CLOSED || state == ConnectionState::CLOSING ||
        readPaused || peerClosed || readBudget == 0) {
        // 连接状态、流控、对端状态或读取预算不允许继续读取，检查是否需要完成关闭。
        if (peerClosed && activityState == ConnectionActivityState::DISCONNECTED && outputQueue.empty()) {
            // 对端已半关闭、连接不再处理请求且无待发响应，完成关闭。
            close();
        }
        // 连接状态、流控或读取预算不允许继续读取，结束本轮处理。
        return;
    }

    std::size_t remaining = readBudget; // 剩余预算：限制本次可从连接读取的字节数
    while (remaining > 0 && state != ConnectionState::CLOSED && state != ConnectionState::CLOSING &&
           !readPaused && !peerClosed) {
        if (pendingInput.size() >= maxBufferedInputBytes) {
            // 输入上限已满：暂停读取，等待在途请求完成后继续解析。
            readPaused = true;
            break;
        }
        const std::size_t amount = std::min(remaining, maxBufferedInputBytes - pendingInput.size()); // 本次读取上限：受预算和输入缓冲容量共同限制
        const std::size_t before = pendingInput.size(); // 读取前长度：用于计算本次新增输入字节
        const TransportResult result = transport->read(pendingInput, amount); // 读取结果：判断是否继续读取或等待事件
        const std::size_t count = pendingInput.size() - before; // 新增长度：本次读取写入输入缓冲的字节数
        remaining -= std::min(remaining, count);
        if (count != 0 && !processInput(executor)) {
            // 本轮新增数据无法继续解析，结束本次读取。
            return;
        }
        if (state == ConnectionState::CLOSED) {
            // 输入处理已关闭连接，不再继续读取。
            return;
        }
        if (result == TransportResult::PEER_CLOSED) {
            // 对端关闭写方向，记录半关闭并排空已接收数据。
            onPeerHalfClose();
            return;
        }
        if (state == ConnectionState::CLOSING) {
            // 连接已进入关闭阶段，停止读取更多输入。
            return;
        }
        if (result == TransportResult::FAILED) {
            // 传输读取发生不可恢复错误，关闭当前连接。
            close();
            return;
        }
        if (result == TransportResult::WOULD_BLOCK) {
            // 当前没有更多可读数据，检查传输层要求的后续就绪方向。
            const std::uint8_t wants = transport->events(); // 等待方向：传输层要求下一次关注的 I/O 事件
            if ((wants & WANT_WRITE) != 0 && (wants & WANT_READ) == 0) {
                // 读操作等待可写事件，记录挂起读操作以便后续重试。
                pendingTransportOperation = PendingTransportOperation::READ;
                pendingTransportBytes = amount;
            }
            break;
        }
        if (count == 0) {
            // 本轮没有读取到新字节，避免在无进展时继续循环。
            break;
        }
    }
}

/*
    函数：onPeerHalfClose
    传参：无
    功能：记录对端半关闭并继续排空已接收的请求和响应
    返回值：无
*/
void Connection::onPeerHalfClose() {
    if (peerClosed || state == ConnectionState::CLOSED ||
        activityState == ConnectionActivityState::DISCONNECTED) {
        // 对端已半关闭或连接已结束，不重复改变连接状态。
        return;
    }
    peerClosed = true;
    readPaused = true;
    closeAfterOutput = true;
    state = ConnectionState::CLOSING;
    if (executorForResume != nullptr) {
        // 存在可用于恢复处理的执行器，继续解析此前缓存的输入。
        processInput(*executorForResume);
    }
}

/*
    函数：processInput
    传参：executor：命令执行器
    功能：把输入缓冲中已完整的帧解析为命令并放入流水线，随后尝试调度
    返回值：连接是否仍然可用
*/
bool Connection::processInput(ICommandExecutor& executor) {
    executorForResume = &executor;
    while (pendingInputOffset < pendingInput.size() &&
           pipeline.size() + (activityState == ConnectionActivityState::PROCESSING ? 1U : 0U) <
               maxPipelineRequests &&
           !pendingProtocolError.has_value() &&
           activityState != ConnectionActivityState::DISCONNECTED &&
           state != ConnectionState::CLOSED &&
           (state != ConnectionState::CLOSING || peerClosed)) {
        Frame frame; // 解码帧：接收 FrameCodec 输出的完整协议帧
        const DecodeStatus status = codec.feed(pendingInput, pendingInputOffset, frame); // 解码状态：区分完整帧、等待更多数据和错误
        if (status == DecodeStatus::NEED_MORE) {
            // 当前输入尚未组成完整帧，保留未消费字节并等待后续数据。
            break;
        }
        if (status != DecodeStatus::FRAME_READY) {
            // 帧非法：无法在字节流中重新同步，只能关闭连接。
            close();
            return false;
        }
        if (!handleFrame(std::move(frame), executor)) {
            // 当前帧未能继续进入请求流水线，停止解析后续输入字节。
            if (state == ConnectionState::CLOSED) {
                // 帧处理已关闭连接，向调用方报告连接不可继续使用。
                return false;
            }
            // 帧处理要求暂停解析，保留后续字节等待状态推进。
            break;
        }
        // 同一连接最多一个在途请求，因此本次解析到此为止，剩余字节留给完成回投后继续处理。
        if (activityState == ConnectionActivityState::PROCESSING) {
            // 当前请求已派发且仍在途，保留后续输入等待完成结果后继续解析。
            break;
        }
    }

    if (pendingInputOffset == pendingInput.size()) {
        // 所有输入字节均已消费，清空缓冲并重置偏移量。
        pendingInput.clear();
        pendingInputOffset = 0;
    } else if (pendingInputOffset >= pendingInput.size() / 2) {
        // 已消费前缀达到缓冲区一半，移除前缀以回收空间。
        pendingInput.erase(pendingInput.begin(),
                           pendingInput.begin() + static_cast<std::ptrdiff_t>(pendingInputOffset));
        pendingInputOffset = 0;
    }
    readPaused = pendingProtocolError.has_value() ||
        pipeline.size() + (activityState == ConnectionActivityState::PROCESSING ? 1U : 0U) >=
            maxPipelineRequests;
    dispatchNext(executor);

    if (peerClosed && activityState == ConnectionActivityState::IDLE && pipeline.empty() &&
        !pendingProtocolError.has_value()) {
        // 对端已半关闭且请求、错误响应队列均已排空，标记连接断开并清除剩余输入。
        activityState = ConnectionActivityState::DISCONNECTED;
        state = ConnectionState::CLOSING;
        pendingInput.clear();
        pendingInputOffset = 0;
        if (outputQueue.empty()) {
            // 已无待发送响应，立即完成连接关闭。
            close();
        }
    }

    return state != ConnectionState::CLOSED;
}

/*
    函数：handleFrame
    传参：frame：已完整解析的协议帧；executor：命令执行器
    功能：校验帧并转换为命令请求放入流水线
    返回值：是否可以继续解析后续帧
*/
bool Connection::handleFrame(Frame frame, ICommandExecutor& executor) {
    CommandRequest request; // 结构化请求：接收解析器输出的命令字段和连接信息
    ErrorCode error = ErrorCode::OK; // 解析错误：保存字段校验失败对应的稳定错误分类
    std::string message; // 错误说明：保存解析器返回的辅助文本
    const RequestId requestId = frame.header.requestId; // 请求标识：关联本帧后续错误或响应
    const Opcode opcode = frame.header.opcode; // 命令码：关联本帧后续错误响应
    if (!parser.parse(std::move(frame), id, request, error, message, static_cast<std::uint32_t>(maxFrameBytes))) {
        // 请求字段校验失败，按序记录协议错误并暂停接收后续请求。
        pendingProtocolError = PendingProtocolError{requestId, opcode, error, std::move(message)};
        readPaused = true;
        return false;
    }
    request.peer = peer;

    pipeline.push_back(std::move(request));
    dispatchNext(executor);
    return true;
}

/*
    函数：queueProtocolError
    传参：requestId：出错请求标识；opcode：出错命令码；error：状态码；message：错误说明
    功能：写出一条协议错误响应并进入关闭流程
    返回值：无
    备注：协议错误后停止处理新请求，按序写完已生成的响应后关闭
*/
void Connection::queueProtocolError(RequestId requestId, Opcode opcode, ErrorCode error, std::string message) {
    Frame frame; // 错误帧：构造与失败请求关联的协议响应
    frame.header.version = static_cast<std::uint8_t>(PROTOCOL_VERSION);
    frame.header.flags = static_cast<std::uint8_t>(RESPONSE | ERROR);
    frame.header.opcode = opcode;
    frame.header.requestId = requestId;
    appendStatus(frame, error);
    TlvField text; // 消息字段：承载协议错误的辅助说明
    text.fieldId = fieldId(FieldId::MESSAGE);
    text.type = FieldType::UTF8;
    const std::string safeMessage = boundedUtf8(message); // 安全文本：限制长度并截断到完整 UTF-8 前缀
    text.value.assign(safeMessage.begin(), safeMessage.end());
    frame.fields.push_back(std::move(text));

    ByteBuffer encoded = codec.encode(frame); // 编码结果：待加入连接输出队列的错误响应帧
    if (encoded.empty() || encoded.size() > MAX_OUTPUT_BYTES - queuedOutputBytes) {
        // 错误响应无法编码或超出输出容量，关闭无法继续返回响应的连接。
        close();
        return;
    }
    queuedOutputBytes += encoded.size();
    outputQueue.push_back(std::move(encoded));
    outputOffsets.push_back(0);
    pendingProtocolError.reset();
    activityState = ConnectionActivityState::DISCONNECTED;
    activeRequestId.reset();
    closeAfterOutput = true;
    readPaused = true;
    pipeline.clear();
    pendingInput.clear();
    pendingInputOffset = 0;
    state = ConnectionState::CLOSING;
}

/*
    函数：onWritable
    传参：writeBudget：本次最多写出的字节数
    功能：按预算写出输出缓冲，必要时推进握手或完成关闭
    返回值：无
*/
void Connection::onWritable(std::size_t writeBudget) {
    if (state == ConnectionState::CLOSED) {
        // 连接已经关闭，不再尝试写出数据。
        return;
    }
    if (state == ConnectionState::TLS_HANDSHAKE) {
        // 连接仍处于 TLS 握手阶段，推进握手状态后结束本次可写处理。
        const TransportResult result = transport->handshake(); // 握手结果：推进当前连接的 TLS 协商
        if (result == TransportResult::OK) {
            // TLS 握手完成，切换到等待 HELLO 协商的阶段。
            state = ConnectionState::WAIT_HELLO;
        } else if (result != TransportResult::WOULD_BLOCK) {
            // TLS 握手遇到不可恢复错误，关闭连接并清理其资源。
            close();
        }
        return;
    }

    if (pendingTransportOperation != PendingTransportOperation::NONE) {
        // 存在挂起传输操作，优先按传输层要求重试，不开始普通写入。
        if ((transport->events() & WANT_WRITE) == 0) {
            // 挂起操作当前不等待可写事件，保留操作并等待目标事件。
            return;
        }
        if (pendingTransportOperation == PendingTransportOperation::READ) {
            // 挂起操作为读取，重试输入并按结果推进连接状态。
            std::size_t bytesRead = 0; // 已读字节数：记录重试读操作新增的输入
            const TransportResult result = retryPendingRead(bytesRead); // 重试结果：决定继续等待或处理连接关闭
            if (bytesRead != 0 && (state != ConnectionState::CLOSING || peerClosed) &&
                !processInput(*executorForResume)) {
                // 新输入无法继续处理，结束本轮写就绪回调。
                return;
            }
            if (state == ConnectionState::CLOSED) {
                // 输入处理已关闭连接，不再推进后续传输操作。
                return;
            }
            if (result == TransportResult::PEER_CLOSED) {
                // 对端关闭写方向，记录半关闭并排空已接收数据。
                onPeerHalfClose();
            } else if (result == TransportResult::FAILED) {
                // 重试读发生不可恢复错误，关闭当前连接。
                close();
                return;
            } else if (result == TransportResult::WOULD_BLOCK) {
                // 重试读仍需等待传输就绪，保留操作并结束本轮处理。
                return;
            }
            if (state == ConnectionState::CLOSING) {
                // 重试读后连接处于关闭阶段，按断开状态清理残留输入。
                if (activityState == ConnectionActivityState::DISCONNECTED) {
                    // 连接已断开，清除不再处理的输入缓存。
                    pendingInput.clear();
                    pendingInputOffset = 0;
                }
            }
        } else {
            // 挂起的是写操作，重试输出并扣减本轮写入预算。
            std::size_t bytesWritten = 0; // 已写字节数：记录重试写操作的进度
            const TransportResult result = retryPendingWrite(bytesWritten); // 重试结果：决定继续等待或收尾
            writeBudget -= std::min(writeBudget, bytesWritten);
            if (result == TransportResult::FAILED || result == TransportResult::PEER_CLOSED) {
                // 重试写失败或对端已关闭，关闭当前连接。
                close();
                return;
            }
            if (result == TransportResult::WOULD_BLOCK) {
                // 重试写仍需等待传输就绪，保留操作并结束本轮处理。
                return;
            }
        }
    }

    while (writeBudget > 0 && !outputQueue.empty()) {
        ByteBuffer& bytes = outputQueue.front(); // 当前帧：输出队列中等待发送的响应字节
        std::size_t& offset = outputOffsets.front(); // 写入进度：当前响应帧已发送的字节数
        const std::size_t amount = std::min(writeBudget, bytes.size() - offset); // 本次写入上限：受写预算和帧剩余长度共同限制
        const std::span<const std::uint8_t> chunk(bytes.data() + offset, amount);
        std::size_t chunkOffset = 0; // 分块进度：本次写操作消耗的字节数
        const TransportResult result = transport->write(chunk, chunkOffset); // 写入结果：说明当前传输是否完成或需要重试
        offset += chunkOffset;
        queuedOutputBytes -= chunkOffset;
        writeBudget -= chunkOffset;
        if (result == TransportResult::FAILED || result == TransportResult::PEER_CLOSED) {
            // 写入失败或对端已关闭，关闭当前连接。
            close();
            return;
        }
        if (offset == bytes.size()) {
            // 当前响应帧已全部发送，从输出队列中移除该帧及其偏移量。
            outputQueue.pop_front();
            outputOffsets.pop_front();
        }
        if (state == ConnectionState::CLOSED) {
            // 传输写入已关闭连接，不再处理本轮输出。
            return;
        }
        if (result == TransportResult::WOULD_BLOCK) {
            // 当前写入需要等待传输就绪，检查下一次重试所需的事件方向。
            const std::uint8_t wants = transport->events(); // 等待方向：传输层要求下一次关注的 I/O 事件
            if (wants == WANT_READ || wants == WANT_WRITE) {
                // 传输层等待方向有效，记录挂起写操作以便后续重试。
                pendingTransportOperation = PendingTransportOperation::WRITE;
                pendingTransportBytes = amount;
            }
            break;
        }
        if (chunkOffset == 0) {
            // 本次写入没有取得进展，避免在无进展时继续循环。
            break;
        }
    }

    if (activityState == ConnectionActivityState::IDLE && (!closeAfterOutput || peerClosed)) {
        // 当前没有在途请求且允许继续处理输入，恢复解析缓冲中的请求。
        if (!peerClosed) {
            // 对端仍可写入，恢复读取以接收后续请求。
            readPaused = false;
        }
        if (!processInput(*executorForResume)) {
            // 缓存输入无法继续处理，结束本轮写就绪回调。
            return;
        }
    }
    if (closeAfterOutput && outputQueue.empty() &&
        activityState == ConnectionActivityState::DISCONNECTED) {
        // 关闭条件成立且输出已排空，完成连接关闭。
        close();
        return;
    }
}

/*
    函数：complete
    传参：response：业务执行结果
    功能：把响应编码进输出缓冲，推进会话状态并调度下一个请求
    返回值：无
    备注：响应必须对应在途请求；无法对应时按协议错误处理并关闭连接
*/
void Connection::complete(CommandResponse response) {
    if (state == ConnectionState::CLOSED ||
        activityState == ConnectionActivityState::DISCONNECTED || !activeRequestId) {
        // 连接已结束或没有有效在途请求，忽略晚到的业务结果。
        return;
    }
    if (response.connectionId != id || response.requestId != *activeRequestId) {
        // 结果无法与在途请求对应：状态不可恢复，按协议错误收尾。
        const RequestId inFlightId = *activeRequestId; // 在途标识：用于生成无法关联响应错误
        const Opcode inFlightOpcode = activeOpcode; // 在途命令：用于标注关联错误响应
        activeRequestId.reset();
        queueProtocolError(inFlightId, inFlightOpcode, ErrorCode::INTERNAL,
                           "response does not match in-flight request");
        return;
    }

    if (peerClosed) {
        // 对端已半关闭，当前响应写出后不再保留连接。
        closeAfterOutput = true;
    }

    if (state == ConnectionState::CLOSING && !peerClosed) {
        // 连接正因本地主动关闭而收尾，不再接纳业务完成结果。
        return;
    }

    const Opcode completedOpcode = activeOpcode; // 已完成命令：用于构造响应并提交会话状态
    if (response.status == ErrorCode::OK && completedOpcode == Opcode::HELLO) {
        // HELLO 成功时提交协商结果，并据此更新连接协议阶段。
        if (!pendingFeatureBits || !session.negotiate(PROTOCOL_VERSION, *pendingFeatureBits)) {
            // HELLO 缺少有效协商能力或协商失败，将结果改为不支持。
            response.status = ErrorCode::UNSUPPORTED;
            response.message = "protocol negotiation failed";
        } else {
            // HELLO 协商成功，切换到等待身份认证的阶段。
            state = ConnectionState::WAIT_AUTH;
        }
    } else if (response.status == ErrorCode::OK && completedOpcode == Opcode::AUTH) {
        // AUTH 成功时提交认证主体；认证失败时不改变会话身份。
        if (pendingAuthentication) {
            // AUTH 执行成功且已准备认证主体，更新会话身份并进入就绪阶段。
            session.authenticate(std::move(pendingPrincipal));
            state = ConnectionState::READY;
        }
    }
    pendingFeatureBits.reset();
    pendingPrincipal.clear();
    pendingAuthentication = false;

    Frame frame; // 响应帧：封装执行结果并关联原请求标识
    frame.header.version = static_cast<std::uint8_t>(PROTOCOL_VERSION);
    frame.header.flags = static_cast<std::uint8_t>(RESPONSE | (response.status == ErrorCode::OK ? 0 : ERROR));
    frame.header.opcode = completedOpcode;
    frame.header.requestId = response.requestId;
    if (hasReservedResponseFields(response.fields)) {
        // 业务结果包含协议保留字段，按内部协议错误关闭连接。
        activeRequestId.reset();
        queueProtocolError(response.requestId, completedOpcode, ErrorCode::INTERNAL,
                           "response used reserved fields");
        return;
    }
    frame.fields = std::move(response.fields);
    appendStatus(frame, response.status);
    if (!response.message.empty()) {
        // 业务结果包含辅助说明，将其编码为 UTF-8 消息字段。
        TlvField message; // 消息字段：编码执行结果的辅助说明
        message.fieldId = fieldId(FieldId::MESSAGE);
        message.type = FieldType::UTF8;
        const std::string safeMessage = boundedUtf8(response.message); // 安全文本：限制长度并截断到完整 UTF-8 前缀
        message.value.assign(safeMessage.begin(), safeMessage.end());
        frame.fields.push_back(std::move(message));
    }

    ByteBuffer encoded = codec.encode(frame); // 编码结果：待加入连接输出队列的响应帧
    if (encoded.empty() || encoded.size() > MAX_OUTPUT_BYTES - queuedOutputBytes) {
        // 响应编码失败或超出输出容量，清除在途标识并关闭连接。
        activeRequestId.reset();
        close();
        return;
    }
    queuedOutputBytes += encoded.size();
    outputQueue.push_back(std::move(encoded));
    outputOffsets.push_back(0);
    const bool closeConnection = response.closeAfterWrite || completedOpcode == Opcode::QUIT; // 关闭决策：响应写完后是否终止连接
    closeAfterOutput = closeAfterOutput || response.closeAfterWrite ||
                       completedOpcode == Opcode::QUIT || peerClosed;
    activeRequestId.reset();
    activityState = ConnectionActivityState::IDLE;

    if (closeConnection) {
        // 响应要求关闭连接，停止派发并清除尚未处理的请求。
        activityState = ConnectionActivityState::DISCONNECTED;
        state = ConnectionState::CLOSING;
        // 终止类响应先结束连接处理，再清理尚未派发的请求。
        pipeline.clear();
        pendingInput.clear();
        pendingInputOffset = 0;
        if (outputQueue.empty()) {
            // 没有待发送响应，立即释放连接资源。
            close();
        }
        return;
    }
    readPaused = false;
    processInput(*executorForResume);
}

/*
    函数：close
    传参：无
    功能：立即停止 I/O、丢弃缓冲并释放连接槽位
    返回值：无
*/
void Connection::close() {
    if (state == ConnectionState::CLOSED) {
        // 连接已完成关闭，避免重复清理和重复释放槽位。
        return;
    }
    activityState = ConnectionActivityState::DISCONNECTED;
    state = ConnectionState::CLOSED;
    pipeline.clear();
    activeRequestId.reset();
    pendingFeatureBits.reset();
    pendingPrincipal.clear();
    pendingAuthentication = false;
    pendingInput.clear();
    pendingInputOffset = 0;
    outputQueue.clear();
    outputOffsets.clear();
    queuedOutputBytes = 0;
    pendingProtocolError.reset();
    pendingTransportOperation = PendingTransportOperation::NONE;
    pendingTransportBytes = 0;
    if (transport) {
        // 存在传输对象，关闭其底层 I/O 资源。
        transport->close();
    }
    if (releaseSlot) {
        // 存在连接槽位释放回调，取出并执行一次以归还容量。
        auto release = std::move(releaseSlot); // 释放回调：取出连接槽位释放动作并确保只调用一次
        release();
    }
}

/*
    函数：dispatchNext
    传参：executor：命令执行器
    功能：把队首请求提交给执行器，保证同一时刻最多一个在途请求
    返回值：无
*/
void Connection::dispatchNext(ICommandExecutor& executor) {
    executorForResume = &executor;
    if (activityState != ConnectionActivityState::IDLE || state == ConnectionState::CLOSED ||
        (state == ConnectionState::CLOSING && !peerClosed)) {
        // 连接忙碌、已关闭或正主动收尾时，不派发新的业务请求。
        return;
    }
    if (pipeline.empty()) {
        // 没有等待请求时，仅处理按序延迟的协议错误并结束派发。
        if (pendingProtocolError.has_value()) {
            // 没有排队请求但存在延迟协议错误，按序生成该错误响应。
            PendingProtocolError error = std::move(*pendingProtocolError); // 延迟错误：按序取出等待生成的协议错误响应
            pendingProtocolError.reset();
            queueProtocolError(error.requestId, error.opcode, error.error, std::move(error.message));
        }
        return;
    }

    CommandRequest request = std::move(pipeline.front()); // 队首请求：按接收顺序移出等待队列准备派发
    pipeline.pop_front();

    if (request.opcode == Opcode::HELLO && session.isNegotiated()) {
        // 会话已完成协商却再次收到 HELLO，按协议错误结束连接。
        queueProtocolError(request.requestId, request.opcode, ErrorCode::BAD_FRAME,
                           "HELLO already completed");
        return;
    }

    request.principal = session.getPrincipal();
    activeOpcode = request.opcode;
    activeRequestId = request.requestId;
    activityState = ConnectionActivityState::PROCESSING;

    if (!session.canExecute(request.opcode, RequestSource::REMOTE)) {
        // 当前会话阶段不允许执行该命令，生成阶段错误而不提交业务执行器。
        pendingFeatureBits.reset();
        pendingPrincipal.clear();
        pendingAuthentication = false;
        CommandResponse response; // 拒绝响应：关联当前请求并报告会话阶段错误
        response.connectionId = id;
        response.requestId = request.requestId;
        response.status = session.isNegotiated() ? ErrorCode::NOAUTH : ErrorCode::BAD_FRAME;
        response.message = session.isNegotiated() ? "authentication required" : "HELLO required";
        complete(std::move(response));
        return;
    }

    pendingFeatureBits.reset();
    pendingPrincipal.clear();
    pendingAuthentication = request.opcode == Opcode::AUTH;
    if (request.opcode == Opcode::HELLO) {
        // 当前请求为 HELLO，保存其协商能力供成功完成时提交会话。
        const TlvField* features = findField(request, FieldId::FEATURE_BITS); // 特性字段：保存待协商的协议能力位
        if (features != nullptr) {
            // 请求包含能力字段，解码并暂存其协商位。
            pendingFeatureBits = readU64(features->value);
        } else {
            // 请求未提供能力字段，按无可选能力处理。
            pendingFeatureBits = 0;
        }
    } else if (pendingAuthentication) {
        // 当前请求为 AUTH，暂存其用户名供认证成功后写入会话。
        const TlvField* username = findField(request, FieldId::USERNAME); // 用户名字段：保存认证成功后使用的主体
        if (username != nullptr) {
            // 请求包含用户名字段，暂存其内容作为认证主体。
            pendingPrincipal.assign(username->value.begin(), username->value.end());
        }
    }

    const ConnectionId connectionId = id; // 连接标识副本：请求移动后仍用于关联异步结果
    const RequestId requestId = request.requestId; // 请求标识副本：请求移动后仍用于关联异步结果
    const auto poster = completionPoster; // 回投路径副本：异步完成时将结果送回所属 Reactor
    try {
        executor.execute(std::move(request), [poster, connectionId, requestId](CommandResponse response) mutable { // response：执行器完成后返回的结构化结果
            if (response.connectionId == 0) {
                // 执行结果未填连接标识，使用派发请求时捕获的连接标识补全。
                response.connectionId = connectionId;
            }
            if (response.requestId == 0) {
                // 执行结果未填请求标识，使用派发请求时捕获的请求标识补全。
                response.requestId = requestId;
            }
            if (poster) {
                // 存在完成回投路径，将业务结果交回所属 Reactor。
                poster(std::move(response));
            }
        });
    } catch (...) {
        pendingFeatureBits.reset();
        pendingPrincipal.clear();
        pendingAuthentication = false;
        CommandResponse response; // 异常响应：将执行器同步拒绝映射为内部错误
        response.connectionId = connectionId;
        response.requestId = requestId;
        response.status = ErrorCode::INTERNAL;
        response.message = "command executor rejected request";
        if (poster) {
            // 存在完成回投路径，将执行器异常转换后的错误结果交回所属 Reactor。
            poster(std::move(response));
        }
    }
}

/*
    函数：interestEvents
    传参：无
    功能：按当前状态给出 Poller 需要关注的事件
    返回值：PollEvent 位掩码；返回 0 表示只等待对端关闭
    备注：Poller 始终订阅对端关闭事件，因此这里无需额外兜底事件
*/
std::uint32_t Connection::interestEvents() const {
    if (state == ConnectionState::CLOSED) {
        // 连接已关闭，不再关注读写事件。
        return 0;
    }
    if (state == ConnectionState::TLS_HANDSHAKE) {
        // TLS 握手期间仅关注传输层要求的就绪方向。
        const std::uint8_t wants = transport->events(); // 等待方向：TLS 握手当前要求的 I/O 就绪类型
        std::uint32_t events = 0; // 事件掩码：转换为 Poller 使用的关注事件
        if ((wants & WANT_READ) != 0) {
            // TLS 握手等待可读时，将读事件加入关注掩码。
            events |= POLL_READ;
        }
        if ((wants & WANT_WRITE) != 0) {
            // TLS 握手等待可写时，将写事件加入关注掩码。
            events |= POLL_WRITE;
        }
        return events;
    }
    if (pendingTransportOperation != PendingTransportOperation::NONE) {
        // 存在挂起传输操作时，仅关注该操作要求的就绪方向。
        const std::uint8_t wants = transport->events(); // 等待方向：挂起传输操作当前要求的 I/O 就绪类型
        std::uint32_t events = 0; // 事件掩码：转换为 Poller 使用的关注事件
        if ((wants & WANT_READ) != 0) {
            // 挂起操作等待可读时，将读事件加入关注掩码。
            events |= POLL_READ;
        }
        if ((wants & WANT_WRITE) != 0) {
            // 挂起操作等待可写时，将写事件加入关注掩码。
            events |= POLL_WRITE;
        }
        return events;
    }
    std::uint32_t events = 0; // 事件掩码：汇总连接当前需要监听的读写事件
    if (!readPaused && !peerClosed && state != ConnectionState::CLOSING &&
        activityState != ConnectionActivityState::DISCONNECTED &&
        pipeline.size() + (activityState == ConnectionActivityState::PROCESSING ? 1U : 0U) <
            maxPipelineRequests) {
        // 连接仍可接收请求且流水线未满，关注可读事件。
        events |= POLL_READ;
    }
    if (!outputQueue.empty() || (transport->events() & WANT_WRITE) != 0) {
        // 存在待发响应或传输层等待写入，关注可写事件。
        events |= POLL_WRITE;
    }
    return events;
}

/*
    函数：getId
    传参：无
    功能：读取连接标识
    返回值：ConnectionId
*/
ConnectionId Connection::getId() const {
    return id;
}

/*
    函数：getState
    传参：无
    功能：读取连接状态
    返回值：ConnectionState
*/
ConnectionState Connection::getState() const {
    return state;
}

}
