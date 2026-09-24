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
#include <utility>

namespace mydb::net {
namespace {

constexpr std::size_t MAX_OUTPUT_BYTES = 16U * 1024U * 1024U; // 输出积压上限：超过即视为不可恢复并关闭连接
constexpr std::size_t MAX_ERROR_MESSAGE_BYTES = 256;          // 错误信息上限：避免把超长文本写回客户端
constexpr std::size_t READ_BUDGET_EXTRA_BYTES = 64U * 1024U;  // 输入缓冲余量：在单帧上限之上允许的未解析字节

std::uint64_t readU64(const ByteBuffer& value) {
    std::uint64_t result = 0;
    for (std::uint8_t byte : value) {
        result = (result << 8U) | byte;
    }
    return result;
}

/*
    函数：boundedUtf8
    参数：message：原始信息
    功能：把信息截断到协议上限，并保证截断后仍是合法 UTF-8
    返回：可直接写入响应帧的文本
*/
std::string boundedUtf8(const std::string& message) {
    const std::size_t length = std::min(message.size(), MAX_ERROR_MESSAGE_BYTES);
    std::size_t validLength = length;
    while (validLength > 0 && !isValidUtf8(reinterpret_cast<const std::uint8_t*>(message.data()), validLength)) {
        --validLength;
    }
    return message.substr(0, validLength);
}

/*
    函数：hasReservedResponseFields
    参数：fields：业务返回的结果字段
    功能：判断结果字段是否占用了协议保留字段
    返回：是否包含保留字段
    备注：状态码和错误信息由网络层统一填充，业务不得自行占用
*/
bool hasReservedResponseFields(const std::vector<TlvField>& fields) {
    return std::any_of(fields.begin(), fields.end(), [](const TlvField& field) {
        return field.fieldId == fieldId(FieldId::STATUS_CODE) || field.fieldId == fieldId(FieldId::MESSAGE);
    });
}

const TlvField* findField(const CommandRequest& request, FieldId id) {
    const auto expected = fieldId(id);
    const auto found = std::find_if(request.fields.begin(), request.fields.end(), [expected](const TlvField& field) {
        return field.fieldId == expected;
    });
    return found == request.fields.end() ? nullptr : &*found;
}

/*
    函数：appendStatus
    参数：frame：待补充的响应帧；code：网络层状态码
    功能：把一个状态码字段追加到响应帧
    返回：无
*/
void appendStatus(Frame& frame, ErrorCode code) {
    TlvField status;
    status.fieldId = fieldId(FieldId::STATUS_CODE);
    status.type = FieldType::U64;
    const auto value = static_cast<std::uint64_t>(code);
    for (int shift = 56; shift >= 0; shift -= 8) {
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
        this->transport->close();
        state = ConnectionState::CLOSED;
        if (this->releaseSlot) {
            auto release = std::move(this->releaseSlot);
            release();
        }
        return;
    }
    const TransportResult result = this->transport->handshake();
    if (result == TransportResult::OK) {
        state = ConnectionState::WAIT_HELLO;
    } else if (result == TransportResult::WOULD_BLOCK) {
        state = ConnectionState::TLS_HANDSHAKE;
    } else {
        state = ConnectionState::CLOSED;
        this->transport->close();
        if (this->releaseSlot) {
            auto release = std::move(this->releaseSlot);
            release();
        }
    }
}

Connection::~Connection() {
    close(false);
}

/*
    函数：onReadable
    参数：executor：命令执行器；readBudget：本次最多读取的字节数
    功能：推进握手或读取输入，并按帧解析后提交命令
    返回：无
    备注：readBudget 为 0 时只解析已缓冲数据，不进行读取
*/
void Connection::onReadable(ICommandExecutor& executor, std::size_t readBudget) {
    executorForResume = &executor;
    if (state == ConnectionState::CLOSED || state == ConnectionState::CLOSING) {
        return;
    }
    if (state == ConnectionState::TLS_HANDSHAKE) {
        const TransportResult result = transport->handshake();
        if (result == TransportResult::OK) {
            state = ConnectionState::WAIT_HELLO;
        } else if (result != TransportResult::WOULD_BLOCK) {
            close(false);
        }
        return;
    }
    if (!processInput(executor)) {
        return;
    }
    if (state == ConnectionState::CLOSED || state == ConnectionState::CLOSING ||
        readPaused || draining || peerClosed || readBudget == 0) {
        return;
    }

    std::size_t remaining = readBudget;
    while (remaining > 0 && state != ConnectionState::CLOSED && state != ConnectionState::CLOSING &&
           !readPaused && !draining && !peerClosed) {
        if (pendingInput.size() >= maxBufferedInputBytes) {
            // 输入上限已满：暂停读取，等待在途请求完成后继续解析。
            readPaused = true;
            break;
        }
        const std::size_t amount = std::min(remaining, maxBufferedInputBytes - pendingInput.size());
        const std::size_t before = pendingInput.size();
        const TransportResult result = transport->read(pendingInput, amount);
        const std::size_t count = pendingInput.size() - before;
        remaining -= std::min(remaining, count);
        if (count != 0 && !processInput(executor)) {
            return;
        }
        if (state == ConnectionState::CLOSED || state == ConnectionState::CLOSING) {
            return;
        }
        if (result == TransportResult::FAILED) {
            close(false);
            return;
        }
        if (result == TransportResult::PEER_CLOSED) {
            peerClosed = true;
            draining = true;
            readPaused = true;
            closeAfterOutput = true;
            if (!activeRequest && pipeline.empty() && outputQueue.empty()) {
                close(false);
            } else {
                state = ConnectionState::CLOSING;
            }
            return;
        }
        if (result == TransportResult::WOULD_BLOCK || count == 0) {
            break;
        }
    }
}

/*
    函数：processInput
    参数：executor：命令执行器
    功能：把输入缓冲中已完整的帧解析为命令并放入流水线，随后尝试调度
    返回：连接是否仍然可用
*/
bool Connection::processInput(ICommandExecutor& executor) {
    executorForResume = &executor;
    while (pendingInputOffset < pendingInput.size() &&
           pipeline.size() + (activeRequest ? 1U : 0U) < maxPipelineRequests &&
           state != ConnectionState::CLOSED && state != ConnectionState::CLOSING) {
        Frame frame;
        const DecodeStatus status = codec.feed(pendingInput, pendingInputOffset, frame);
        if (status == DecodeStatus::NEED_MORE) {
            break;
        }
        if (status != DecodeStatus::FRAME_READY) {
            // 帧非法：无法在字节流中重新同步，只能关闭连接。
            close(false);
            return false;
        }
        if (!handleFrame(std::move(frame), executor)) {
            if (state == ConnectionState::CLOSED) {
                return false;
            }
            break;
        }
        // 同一连接最多一个在途请求，因此本次解析到此为止，剩余字节留给完成回投后继续处理。
        if (activeRequest) {
            break;
        }
    }

    if (pendingInputOffset == pendingInput.size()) {
        pendingInput.clear();
        pendingInputOffset = 0;
    } else if (pendingInputOffset >= pendingInput.size() / 2) {
        pendingInput.erase(pendingInput.begin(),
                           pendingInput.begin() + static_cast<std::ptrdiff_t>(pendingInputOffset));
        pendingInputOffset = 0;
    }
    readPaused = pipeline.size() + (activeRequest ? 1U : 0U) >= maxPipelineRequests;
    dispatchNext(executor);
    return state != ConnectionState::CLOSED;
}

/*
    函数：handleFrame
    参数：frame：已完整解析的协议帧；executor：命令执行器
    功能：校验帧并转换为命令请求放入流水线
    返回：是否可以继续解析后续帧
*/
bool Connection::handleFrame(Frame frame, ICommandExecutor& executor) {
    CommandRequest request;
    ErrorCode error = ErrorCode::OK;
    std::string message;
    if (!parser.parse(frame, id, request, error, message, static_cast<std::uint32_t>(maxFrameBytes))) {
        queueProtocolError(frame.header.requestId, frame.header.opcode, error, std::move(message));
        return false;
    }
    request.principal = session.getPrincipal();
    request.peer = peer;

    if (request.opcode == Opcode::HELLO && session.isNegotiated()) {
        queueProtocolError(request.requestId, request.opcode, ErrorCode::BAD_FRAME,
                           "HELLO already completed");
        return false;
    }
    if (request.opcode == Opcode::AUTH && !session.isNegotiated()) {
        queueProtocolError(request.requestId, request.opcode, ErrorCode::BAD_FRAME,
                           "HELLO required before AUTH");
        return false;
    }
    pipeline.push_back(std::move(request));
    dispatchNext(executor);
    return true;
}

/*
    函数：queueProtocolError
    参数：requestId：出错请求标识；opcode：出错命令码；error：状态码；message：错误说明
    功能：写出一条协议错误响应并进入关闭流程
    返回：无
    备注：协议错误后不再接受新请求，只把当前错误响应写完
*/
void Connection::queueProtocolError(RequestId requestId, Opcode opcode, ErrorCode error, std::string message) {
    Frame frame;
    frame.header.version = static_cast<std::uint8_t>(PROTOCOL_VERSION);
    frame.header.flags = static_cast<std::uint8_t>(RESPONSE | ERROR);
    frame.header.opcode = opcode;
    frame.header.requestId = requestId;
    appendStatus(frame, error);
    TlvField text;
    text.fieldId = fieldId(FieldId::MESSAGE);
    text.type = FieldType::UTF8;
    const std::string safeMessage = boundedUtf8(message);
    text.value.assign(safeMessage.begin(), safeMessage.end());
    frame.fields.push_back(std::move(text));

    ByteBuffer encoded = codec.encode(frame);
    if (encoded.empty() || encoded.size() > MAX_OUTPUT_BYTES - queuedOutputBytes) {
        close(false);
        return;
    }
    queuedOutputBytes += encoded.size();
    outputQueue.push_back(std::move(encoded));
    outputOffsets.push_back(0);
    closeAfterOutput = true;
    readPaused = true;
    pipeline.clear();
    pendingInput.clear();
    pendingInputOffset = 0;
    state = ConnectionState::CLOSING;
}

/*
    函数：onWritable
    参数：writeBudget：本次最多写出的字节数
    功能：按预算写出输出缓冲，必要时推进握手或触发关闭
    返回：无
*/
void Connection::onWritable(std::size_t writeBudget) {
    if (state == ConnectionState::CLOSED) {
        return;
    }
    if (state == ConnectionState::TLS_HANDSHAKE) {
        const TransportResult result = transport->handshake();
        if (result == TransportResult::OK) {
            state = ConnectionState::WAIT_HELLO;
        } else if (result != TransportResult::WOULD_BLOCK) {
            close(false);
        }
        return;
    }

    while (writeBudget > 0 && !outputQueue.empty()) {
        ByteBuffer& bytes = outputQueue.front();
        std::size_t& offset = outputOffsets.front();
        const std::size_t amount = std::min(writeBudget, bytes.size() - offset);
        ByteBuffer chunk(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                         bytes.begin() + static_cast<std::ptrdiff_t>(offset + amount));
        std::size_t chunkOffset = 0;
        const TransportResult result = transport->write(chunk, chunkOffset);
        offset += chunkOffset;
        queuedOutputBytes -= chunkOffset;
        writeBudget -= chunkOffset;
        if (result == TransportResult::FAILED || result == TransportResult::PEER_CLOSED) {
            close(false);
            return;
        }
        if (offset == bytes.size()) {
            outputQueue.pop_front();
            outputOffsets.pop_front();
        }
        if (result == TransportResult::WOULD_BLOCK || chunkOffset == 0) {
            break;
        }
    }

    if (closeAfterOutput && outputQueue.empty() && !activeRequest) {
        close(false);
        return;
    }
    if (peerClosed && !activeRequest && pipeline.empty() && outputQueue.empty()) {
        close(false);
        return;
    }
    if (!draining && !closeAfterOutput && !activeRequest) {
        // 输出腾出空间后恢复解析输入，并调度下一个待执行请求。
        readPaused = false;
        if (!processInput(*executorForResume)) {
            return;
        }
    }
}

/*
    函数：complete
    参数：response：业务执行结果
    功能：把响应编码进输出缓冲，推进会话状态并调度下一个请求
    返回：无
    备注：响应必须对应在途请求；无法对应时按协议错误处理并关闭连接
*/
void Connection::complete(CommandResponse response) {
    if (state == ConnectionState::CLOSED || !activeRequest) {
        return;
    }
    if (response.connectionId != id || response.requestId != activeRequest->requestId) {
        // 结果无法与在途请求对应：状态不可恢复，按协议错误收尾。
        const RequestId inFlightId = activeRequest->requestId;
        const Opcode inFlightOpcode = activeRequest->opcode;
        activeRequest.reset();
        queueProtocolError(inFlightId, inFlightOpcode, ErrorCode::INTERNAL,
                           "response does not match in-flight request");
        return;
    }

    const Opcode completedOpcode = activeRequest->opcode;
    if (response.status == ErrorCode::OK && completedOpcode == Opcode::HELLO) {
        const TlvField* version = findField(*activeRequest, FieldId::PROTOCOL_VERSION);
        const TlvField* features = findField(*activeRequest, FieldId::FEATURE_BITS);
        if (version == nullptr || version->value.size() != 2 ||
            !session.negotiate(static_cast<std::uint16_t>((version->value[0] << 8U) | version->value[1]),
                               features == nullptr ? 0 : readU64(features->value))) {
            response.status = ErrorCode::UNSUPPORTED;
            response.message = "protocol negotiation failed";
        } else {
            state = ConnectionState::WAIT_AUTH;
        }
    } else if (response.status == ErrorCode::OK && completedOpcode == Opcode::AUTH) {
        const TlvField* username = findField(*activeRequest, FieldId::USERNAME);
        if (username != nullptr) {
            session.authenticate(std::string(username->value.begin(), username->value.end()));
            state = ConnectionState::READY;
        }
    }

    Frame frame;
    frame.header.version = static_cast<std::uint8_t>(PROTOCOL_VERSION);
    frame.header.flags = static_cast<std::uint8_t>(RESPONSE | (response.status == ErrorCode::OK ? 0 : ERROR));
    frame.header.opcode = completedOpcode;
    frame.header.requestId = response.requestId;
    if (hasReservedResponseFields(response.fields)) {
        activeRequest.reset();
        queueProtocolError(response.requestId, completedOpcode, ErrorCode::INTERNAL,
                           "response used reserved fields");
        return;
    }
    frame.fields = std::move(response.fields);
    appendStatus(frame, response.status);
    if (!response.message.empty()) {
        TlvField message;
        message.fieldId = fieldId(FieldId::MESSAGE);
        message.type = FieldType::UTF8;
        const std::string safeMessage = boundedUtf8(response.message);
        message.value.assign(safeMessage.begin(), safeMessage.end());
        frame.fields.push_back(std::move(message));
    }

    ByteBuffer encoded = codec.encode(frame);
    if (encoded.empty() || encoded.size() > MAX_OUTPUT_BYTES - queuedOutputBytes) {
        activeRequest.reset();
        close(false);
        return;
    }
    queuedOutputBytes += encoded.size();
    outputQueue.push_back(std::move(encoded));
    outputOffsets.push_back(0);
    closeAfterOutput = closeAfterOutput || response.closeAfterWrite ||
                       completedOpcode == Opcode::QUIT || peerClosed;
    activeRequest.reset();

    if (closeAfterOutput) {
        // 写完剩余响应即关闭；后续未执行请求直接丢弃。
        state = ConnectionState::CLOSING;
        pipeline.clear();
        pendingInput.clear();
        pendingInputOffset = 0;
        if (outputQueue.empty()) {
            close(false);
        }
        return;
    }
    if (draining) {
        // 排空期间不再读取新输入，只把已接收的请求依次执行完。
        if (!pipeline.empty()) {
            dispatchNext(*executorForResume);
            return;
        }
        closeAfterOutput = true;
        state = ConnectionState::CLOSING;
        if (outputQueue.empty()) {
            close(false);
        }
        return;
    }
    readPaused = false;
    processInput(*executorForResume);
}

/*
    函数：close
    参数：graceful：是否优雅关闭
    功能：立即停止 I/O、丢弃缓冲并释放连接槽位
    返回：无
    备注：优雅关闭由 beginDrain 驱动，本函数只做终态释放，可重复调用
*/
void Connection::close(bool graceful) {
    (void)graceful;
    if (state == ConnectionState::CLOSED) {
        return;
    }
    state = ConnectionState::CLOSED;
    pipeline.clear();
    activeRequest.reset();
    pendingInput.clear();
    pendingInputOffset = 0;
    outputQueue.clear();
    outputOffsets.clear();
    queuedOutputBytes = 0;
    if (transport) {
        transport->close();
    }
    if (releaseSlot) {
        auto release = std::move(releaseSlot);
        release();
    }
}

/*
    函数：beginDrain
    参数：executor：命令执行器
    功能：进入排空状态，停止读取并提交已接收但未执行的请求
    返回：无
    备注：排空期间仍接收停止前已提交请求的完成结果
*/
void Connection::beginDrain(ICommandExecutor& executor) {
    draining = true;
    readPaused = true;
    executorForResume = &executor;
    pendingInput.clear();
    pendingInputOffset = 0;
    if (!activeRequest && !pipeline.empty()) {
        dispatchNext(executor);
    }
    if (!activeRequest && pipeline.empty()) {
        closeAfterOutput = true;
        state = ConnectionState::CLOSING;
        if (outputQueue.empty()) {
            close(false);
        }
    }
}

/*
    函数：dispatchNext
    参数：executor：命令执行器
    功能：把队首请求提交给执行器，保证同一时刻最多一个在途请求
    返回：无
*/
void Connection::dispatchNext(ICommandExecutor& executor) {
    executorForResume = &executor;
    if (state == ConnectionState::CLOSED || activeRequest || pipeline.empty() ||
        (state == ConnectionState::CLOSING && !draining)) {
        return;
    }
    activeRequest = std::move(pipeline.front());
    pipeline.pop_front();

    if (!session.canExecute(activeRequest->opcode, RequestSource::REMOTE)) {
        CommandResponse response;
        response.connectionId = id;
        response.requestId = activeRequest->requestId;
        response.status = session.isNegotiated() ? ErrorCode::NOAUTH : ErrorCode::BAD_FRAME;
        response.message = session.isNegotiated() ? "authentication required" : "HELLO required";
        complete(std::move(response));
        return;
    }

    const CommandRequest request = *activeRequest;
    const auto poster = completionPoster;
    const ConnectionId connectionId = id;
    const RequestId requestId = request.requestId;
    try {
        executor.execute(request, [poster, connectionId, requestId](CommandResponse response) mutable {
            if (response.connectionId == 0) {
                response.connectionId = connectionId;
            }
            if (response.requestId == 0) {
                response.requestId = requestId;
            }
            if (poster) {
                poster(std::move(response));
            }
        });
    } catch (...) {
        CommandResponse response;
        response.connectionId = connectionId;
        response.requestId = requestId;
        response.status = ErrorCode::INTERNAL;
        response.message = "command executor rejected request";
        if (poster) {
            poster(std::move(response));
        }
    }
}

/*
    函数：interestEvents
    参数：无
    功能：按当前状态给出 Poller 需要关注的事件
    返回：PollEvent 位掩码；返回 0 表示只等待对端关闭
    备注：Poller 始终订阅对端关闭事件，因此这里无需额外兜底事件
*/
std::uint32_t Connection::interestEvents() const {
    if (state == ConnectionState::CLOSED) {
        return 0;
    }
    if (state == ConnectionState::TLS_HANDSHAKE) {
        const std::uint8_t wants = transport->events();
        std::uint32_t events = 0;
        if ((wants & WANT_READ) != 0) {
            events |= POLL_READ;
        }
        if ((wants & WANT_WRITE) != 0) {
            events |= POLL_WRITE;
        }
        return events;
    }
    std::uint32_t events = 0;
    if (!readPaused && !draining && !peerClosed && state != ConnectionState::CLOSING &&
        pipeline.size() + (activeRequest ? 1U : 0U) < maxPipelineRequests) {
        events |= POLL_READ;
    }
    if (!outputQueue.empty() || (transport->events() & WANT_WRITE) != 0) {
        events |= POLL_WRITE;
    }
    return events;
}

/*
    函数：getId
    参数：无
    功能：读取连接标识
    返回：ConnectionId
*/
ConnectionId Connection::getId() const {
    return id;
}

/*
    函数：getState
    参数：无
    功能：读取连接状态
    返回：ConnectionState
*/
ConnectionState Connection::getState() const {
    return state;
}

}
