/*
    模块名：网络层正确性测试
    模块地位：网络层协议核心、连接状态机及 Linux 服务端集成的验收入口
    模块功能描述：验证帧格式、命令字段约束、会话授权、连接异步顺序和真实回环网络收发。
*/

#include "net_test_support.h"

#include "net_level/reactor/epoll_poller.h"
#include "net_level/server/connection.h"
#include "net_level/server/network_server.h"
#include "net_level/transport/tls_transport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__) && !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

using namespace mydb::net;
using namespace mydb::net::test;

namespace {

/*
    功能：构造包含协议版本字段的 HELLO 请求
    传参：requestId：请求关联标识
    返回值：构造完成的 HELLO 帧
*/
Frame makeHelloFrame(RequestId requestId) {
    std::vector<TlvField> fields;
    fields.push_back(makeField(fieldId(FieldId::PROTOCOL_VERSION), FieldType::BYTES, {0, 1}));
    return makeFrame(Opcode::HELLO, requestId, std::move(fields));
}

/*
    功能：构造符合协议字段规则的 AUTH 请求
    传参：requestId：请求关联标识
    返回值：构造完成的 AUTH 帧
*/
Frame makeAuthFrame(RequestId requestId) {
    std::vector<TlvField> fields;
    fields.push_back(makeTextBytesField(FieldId::USERNAME, FieldType::UTF8, "tester"));
    fields.push_back(makeTextBytesField(FieldId::PASSWORD, FieldType::BYTES, "secret"));
    return makeFrame(Opcode::AUTH, requestId, std::move(fields));
}

/*
    功能：逐帧解析完整的连续网络字节流
    传参：bytes：包含一个或多个完整帧的字节流；name：失败用例名称
    返回值：按字节流顺序还原的帧列表
*/
std::vector<Frame> decodeFrames(const ByteBuffer& bytes, std::string_view name) {
    FrameCodec decoder;
    std::vector<Frame> frames;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        Frame frame;
        require(decoder.feed(bytes, offset, frame) == DecodeStatus::FRAME_READY, name);
        frames.push_back(std::move(frame));
    }
    return frames;
}

/*
    功能：验证帧编解码和增量流边界
    传参：无
    返回值：无
*/
void testFrameCodec() {
    std::vector<TlvField> fields;
    fields.push_back(makeTextBytesField(FieldId::PAYLOAD, FieldType::BYTES,
                                        std::string_view("A\0B", 3)));
    const Frame source = makeFrame(Opcode::PING, 77, std::move(fields));
    const ByteBuffer encoded = encodeFrame(source, "encode binary-safe payload");

    FrameCodec bytewiseDecoder;
    Frame decoded;
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const ByteBuffer piece{encoded[index]};
        std::size_t offset = 0;
        const DecodeStatus status = bytewiseDecoder.feed(piece, offset, decoded);
        require(status == (index + 1 == encoded.size() ? DecodeStatus::FRAME_READY : DecodeStatus::NEED_MORE),
                "decode frame one byte at a time");
        require(offset == piece.size(), "consume each fragmented input byte");
    }
    require(decoded.header.requestId == 77 && decoded.header.opcode == Opcode::PING,
            "round-trip frame metadata");
    require(decoded.fields.size() == 1 && decoded.fields.front().value == ByteBuffer{'A', 0, 'B'},
            "round-trip binary field including zero byte");

    ByteBuffer coalesced = encoded;
    const ByteBuffer second = encodeFrame(makeFrame(Opcode::PING, 78), "encode second coalesced frame");
    const ByteBuffer third = encodeFrame(makeFrame(Opcode::QUIT, 79), "encode third coalesced frame");
    coalesced.insert(coalesced.end(), second.begin(), second.end());
    coalesced.insert(coalesced.end(), third.begin(), third.end());
    const std::vector<Frame> coalescedFrames = decodeFrames(coalesced, "decode coalesced frames");
    require(coalescedFrames.size() == 3, "coalesced frame count");
    require(coalescedFrames[0].header.requestId == 77 && coalescedFrames[1].header.requestId == 78 &&
                coalescedFrames[2].header.requestId == 79,
            "coalesced frame boundaries and order");

    ByteBuffer invalidMagic = encoded;
    invalidMagic[0] = static_cast<std::uint8_t>('X');
    FrameCodec invalidMagicDecoder;
    std::size_t invalidOffset = 0;
    require(invalidMagicDecoder.feed(invalidMagic, invalidOffset, decoded) == DecodeStatus::INVALID_FRAME,
            "reject invalid protocol magic");

    ByteBuffer invalidFlags = encoded;
    invalidFlags[5] = 0x80;
    FrameCodec invalidFlagsDecoder;
    invalidOffset = 0;
    require(invalidFlagsDecoder.feed(invalidFlags, invalidOffset, decoded) == DecodeStatus::INVALID_FRAME,
            "reject unknown frame flags");

    ByteBuffer invalidUtf8 = encodeFrame(
        makeFrame(Opcode::PING, 80,
                  {makeTextBytesField(FieldId::PAYLOAD, FieldType::BYTES, std::string_view("\xc0", 1))}),
        "encode binary payload for UTF-8 corruption case");
    invalidUtf8[26] = static_cast<std::uint8_t>(FieldType::UTF8);
    FrameCodec invalidUtf8Decoder;
    invalidOffset = 0;
    require(invalidUtf8Decoder.feed(invalidUtf8, invalidOffset, decoded) == DecodeStatus::INVALID_FRAME,
            "reject malformed UTF-8 on decode");

    FrameCodec boundedDecoder(34);
    invalidOffset = 0;
    require(boundedDecoder.feed(encoded, invalidOffset, decoded) == DecodeStatus::FRAME_TOO_LARGE,
            "reject frame beyond configured size");

    Frame invalidText = makeFrame(
        Opcode::PING, 81,
        {makeTextBytesField(FieldId::PAYLOAD, FieldType::UTF8, std::string_view("\xc0", 1))});
    require(FrameCodec{}.encode(invalidText).empty(), "reject malformed UTF-8 on encode");
}

/*
    功能：检查命令解析失败时返回的稳定错误分类
    传参：frame：待解析帧；expected：预期错误；name：用例名称
    返回值：无
*/
void expectParseFailure(Frame frame, ErrorCode expected, std::string_view name,
                        std::uint32_t maxFrameBytes = DEFAULT_MAX_FRAME_BYTES) {
    CommandParser parser;
    CommandRequest request;
    ErrorCode error = ErrorCode::OK;
    std::string message;
    require(!parser.parse(std::move(frame), 9, request, error, message, maxFrameBytes), name);
    require(error == expected, "command parser error mapping");
    require(!message.empty(), "command parser supplies failure message");
}

/*
    功能：覆盖协议 v1 已注册命令的字段校验和边界错误
    传参：无
    返回值：无
*/
void testCommandParser() {
    const std::array<Frame, 14> validFrames{
        makeHelloFrame(1),
        makeAuthFrame(2),
        makeFrame(Opcode::PING, 3, {makeTextBytesField(FieldId::PAYLOAD, FieldType::BYTES, "ping")}),
        makeFrame(Opcode::QUIT, 4),
        makeFrame(Opcode::GET, 5, {makeTextBytesField(FieldId::KEY, FieldType::BYTES, "key")}),
        makeFrame(Opcode::ADD, 6,
                  {makeTextBytesField(FieldId::KEY, FieldType::BYTES, "key"),
                   makeTextBytesField(FieldId::TYPE_NAME, FieldType::UTF8, "text"),
                   makeTextBytesField(FieldId::VALUE, FieldType::BYTES, "value"),
                   makeU64Field(FieldId::TTL, 100)}),
        makeFrame(Opcode::UPDATE, 7,
                  {makeTextBytesField(FieldId::KEY, FieldType::BYTES, "key"),
                   makeTextBytesField(FieldId::TYPE_NAME, FieldType::UTF8, "text"),
                   makeTextBytesField(FieldId::VALUE, FieldType::BYTES, "value")}),
        makeFrame(Opcode::DELETE_DATA, 8, {makeTextBytesField(FieldId::KEY, FieldType::BYTES, "key")}),
        makeFrame(Opcode::PERSIST, 9),
        makeFrame(Opcode::FLUSH, 10),
        makeFrame(Opcode::REWRITE, 11),
        makeFrame(Opcode::STATS, 12, {makeTextBytesField(FieldId::SCOPE, FieldType::UTF8, "network")}),
        makeFrame(Opcode::CLIENT_LIST, 13),
        makeFrame(Opcode::CLIENT_KILL, 14, {makeU64Field(FieldId::CONNECTION_ID, 17)})
    };

    CommandParser parser;
    for (std::size_t index = 0; index < validFrames.size(); ++index) {
        CommandRequest request;
        ErrorCode error = ErrorCode::INTERNAL;
        std::string message = "stale message";
        Frame frame = validFrames[index];
        require(parser.parse(std::move(frame), 42, request, error, message), "parse valid registered opcode");
        require(request.connectionId == 42 && request.requestId == index + 1,
                "preserve connection and request identifiers");
        require(request.opcode == validFrames[index].header.opcode && request.source == RequestSource::REMOTE,
                "preserve parsed opcode and remote source");
        require(error == ErrorCode::OK && message.empty(), "clear parser error on success");
    }

    expectParseFailure(makeFrame(Opcode::GET, 20), ErrorCode::BAD_FRAME, "reject missing required key");
    expectParseFailure(makeFrame(Opcode::GET, 21,
                                 {makeTextBytesField(FieldId::KEY, FieldType::BYTES, "a"),
                                  makeTextBytesField(FieldId::KEY, FieldType::BYTES, "b")}),
                       ErrorCode::BAD_FRAME, "reject duplicate command field");
    expectParseFailure(makeFrame(Opcode::GET, 22,
                                 {makeTextBytesField(FieldId::KEY, FieldType::UTF8, "key")}),
                       ErrorCode::BAD_FRAME, "reject wrong field type");
    expectParseFailure(makeFrame(static_cast<Opcode>(0x7777), 23), ErrorCode::UNKNOWN_COMMAND,
                       "reject unknown opcode");
    expectParseFailure(makeFrame(Opcode::HELLO, 24,
                                 {makeField(fieldId(FieldId::PROTOCOL_VERSION), FieldType::BYTES, {0, 2})}),
                       ErrorCode::UNSUPPORTED, "reject unsupported protocol version");

    Frame responseFrame = makeFrame(Opcode::PING, 25);
    responseFrame.header.flags = RESPONSE;
    expectParseFailure(std::move(responseFrame), ErrorCode::BAD_FRAME, "reject response frame as request");
    expectParseFailure(makeHelloFrame(26), ErrorCode::BAD_FRAME, "reject request exceeding parser frame limit",
                       BASE_HEADER_LENGTH);
}

/*
    功能：验证协商、认证和本地管理权限状态
    传参：无
    返回值：无
*/
void testSession() {
    Session session;
    require(!session.isNegotiated() && !session.isAuthenticated(), "new session starts unauthenticated");
    require(session.canExecute(Opcode::HELLO, RequestSource::REMOTE) &&
                session.canExecute(Opcode::PING, RequestSource::REMOTE) &&
                session.canExecute(Opcode::QUIT, RequestSource::REMOTE),
            "pre-negotiation commands are restricted to handshake and liveness");
    require(!session.canExecute(Opcode::AUTH, RequestSource::REMOTE) &&
                !session.canExecute(Opcode::GET, RequestSource::REMOTE),
            "pre-negotiation session rejects auth and data commands");
    require(session.canExecute(Opcode::GET, RequestSource::LOCAL_ADMIN),
            "local administration bypasses remote session gate");

    require(!session.negotiate(PROTOCOL_VERSION + 1, 7), "reject unsupported session version");
    require(session.negotiate(PROTOCOL_VERSION, 7), "accept supported session version");
    require(session.isNegotiated() && session.getFeatureBits() == 7, "retain negotiated session features");
    require(session.canExecute(Opcode::AUTH, RequestSource::REMOTE) &&
                !session.canExecute(Opcode::GET, RequestSource::REMOTE),
            "pre-authentication session permits only auth and liveness");

    session.authenticate("tester");
    require(session.isAuthenticated() && session.getPrincipal() == "tester", "retain authenticated principal");
    require(session.canExecute(Opcode::GET, RequestSource::REMOTE), "authenticated session permits data command");
}

/*
    功能：按指定请求索引完成执行器回调并将结果交回连接
    传参：connection：被测连接；executor：记录执行器；index：请求序号；posted：完成投递记录；status：响应状态
    返回值：无
*/
void completeRequest(Connection& connection, RecordingExecutor& executor, std::size_t index,
                     std::vector<CommandResponse>& posted, ErrorCode status = ErrorCode::OK) {
    CommandResponse response;
    response.connectionId = executor.requests.at(index).connectionId;
    response.requestId = executor.requests.at(index).requestId;
    response.status = status;
    CommandCompletion completion = std::move(executor.completions.at(index));
    completion(std::move(response));
    require(posted.size() == 1, "executor completion returns through poster");
    connection.complete(std::move(posted.back()));
    posted.pop_back();
}

/*
    功能：验证连接会话推进、异步单在途请求和响应关联顺序
    传参：无
    返回值：无
*/
void testConnectionOrdering() {
    ByteBuffer input = encodeFrame(makeHelloFrame(100), "encode pipelined HELLO");
    const ByteBuffer authBytes = encodeFrame(makeAuthFrame(101), "encode pipelined AUTH");
    const ByteBuffer getBytes = encodeFrame(
        makeFrame(Opcode::GET, 102, {makeTextBytesField(FieldId::KEY, FieldType::BYTES, "key")}),
        "encode pipelined GET");
    input.insert(input.end(), authBytes.begin(), authBytes.end());
    input.insert(input.end(), getBytes.begin(), getBytes.end());

    auto transport = std::make_unique<MemoryTransport>(std::move(input), 5, 3);
    MemoryTransport* transportView = transport.get();
    RecordingExecutor executor;
    std::vector<CommandResponse> posted;
    std::size_t releasedSlots = 0;
    Connection connection(51, 1, std::move(transport), 4, Endpoint{"127.0.0.1", 1234},
                         DEFAULT_MAX_FRAME_BYTES, 7, [&releasedSlots]() { ++releasedSlots; },
                         [&posted](CommandResponse response) { posted.push_back(std::move(response)); });

    require(connection.getState() == ConnectionState::WAIT_HELLO, "plain transport enters HELLO state");
    connection.onReadable(executor, 4096);
    require(executor.requests.size() == 1 && executor.requests.front().opcode == Opcode::HELLO,
            "only first pipelined request is dispatched");

    completeRequest(connection, executor, 0, posted);
    require(executor.requests.size() == 2 && executor.requests[1].opcode == Opcode::AUTH,
            "HELLO completion unlocks next pipelined AUTH");
    completeRequest(connection, executor, 1, posted);
    require(executor.requests.size() == 3 && executor.requests[2].opcode == Opcode::GET,
            "AUTH completion unlocks next pipelined data request");
    require(executor.requests[2].principal == "tester", "authenticated principal reaches subsequent request");
    completeRequest(connection, executor, 2, posted);
    require(connection.getState() == ConnectionState::READY, "connection remains ready after ordered requests");

    for (std::size_t attempt = 0; attempt < 512; ++attempt) {
        connection.onWritable(1);
    }
    const std::vector<Frame> responses = decodeFrames(transportView->writtenBytes(), "decode connection responses");
    require(responses.size() == 3, "one response emitted per request");
    for (std::size_t index = 0; index < responses.size(); ++index) {
        require(responses[index].header.requestId == 100 + index, "response request identifiers remain ordered");
        require((responses[index].header.flags & RESPONSE) != 0 && (responses[index].header.flags & ERROR) == 0,
                "successful response flags");
    }

    connection.close();
    require(connection.getState() == ConnectionState::CLOSED && transportView->isClosed(),
            "explicit close releases transport");
    require(releasedSlots == 1, "connection slot is released exactly once");
}

/*
    功能：验证 HELLO 失败后按序拒绝已排队的 AUTH 和 GET 请求
    传参：无
    返回值：无
*/
void testConnectionRejectsQueuedRequestsAfterHelloFailure() {
    ByteBuffer input = encodeFrame(makeHelloFrame(400), "encode failed pipelined HELLO");
    const ByteBuffer authBytes = encodeFrame(makeAuthFrame(401), "encode queued AUTH after failed HELLO");
    const ByteBuffer getBytes = encodeFrame(
        makeFrame(Opcode::GET, 402, {makeTextBytesField(FieldId::KEY, FieldType::BYTES, "key")}),
        "encode queued GET after failed HELLO");
    input.insert(input.end(), authBytes.begin(), authBytes.end());
    input.insert(input.end(), getBytes.begin(), getBytes.end());

    auto transport = std::make_unique<MemoryTransport>(std::move(input), 5, 3);
    MemoryTransport* transportView = transport.get();
    RecordingExecutor executor;
    std::vector<CommandResponse> posted;
    Connection connection(53, 1, std::move(transport), 4, Endpoint{"127.0.0.1", 1234},
                         DEFAULT_MAX_FRAME_BYTES, 7, {},
                         [&posted](CommandResponse response) { posted.push_back(std::move(response)); });

    connection.onReadable(executor, 4096);
    require(executor.requests.size() == 1 && executor.requests.front().opcode == Opcode::HELLO,
            "HELLO failure remains the only business request dispatched");
    completeRequest(connection, executor, 0, posted, ErrorCode::UNSUPPORTED);
    require(connection.getState() == ConnectionState::WAIT_HELLO,
            "failed HELLO does not advance the connection protocol state");
    require(executor.requests.size() == 1, "queued AUTH and GET are rejected before business dispatch");

    for (std::size_t attempt = 0; attempt < 512; ++attempt) {
        connection.onWritable(1);
    }
    const std::vector<Frame> responses = decodeFrames(transportView->writtenBytes(),
                                                       "decode responses after failed HELLO");
    require(responses.size() == 3, "failed HELLO and queued requests each receive a response");
    for (std::size_t index = 0; index < responses.size(); ++index) {
        require(responses[index].header.requestId == 400 + index,
                "failed HELLO responses preserve request order");
        require((responses[index].header.flags & RESPONSE) != 0 &&
                    (responses[index].header.flags & ERROR) != 0,
                "failed HELLO and later pre-negotiation requests return errors");
    }

    connection.close();
}

/*
    功能：验证排队请求完成前协议解析错误不会越序响应
    传参：无
    返回值：无
*/
void testConnectionDefersProtocolErrorAfterQueuedRequest() {
    ByteBuffer input = encodeFrame(makeHelloFrame(410), "encode HELLO before deferred protocol error");
    const ByteBuffer authBytes = encodeFrame(makeAuthFrame(411), "encode AUTH before deferred protocol error");
    const ByteBuffer invalidGetBytes = encodeFrame(makeFrame(Opcode::GET, 412),
                                                   "encode invalid GET after queued AUTH");
    input.insert(input.end(), authBytes.begin(), authBytes.end());
    input.insert(input.end(), invalidGetBytes.begin(), invalidGetBytes.end());

    auto transport = std::make_unique<MemoryTransport>(std::move(input));
    MemoryTransport* transportView = transport.get();
    RecordingExecutor executor;
    std::vector<CommandResponse> posted;
    Connection connection(54, 1, std::move(transport), 4, Endpoint{"127.0.0.1", 1234},
                         DEFAULT_MAX_FRAME_BYTES, 7, {},
                         [&posted](CommandResponse response) { posted.push_back(std::move(response)); });

    connection.onReadable(executor, 4096);
    connection.onReadable(executor, 4096);
    connection.onReadable(executor, 4096);
    require(executor.requests.size() == 1 && executor.requests.front().opcode == Opcode::HELLO,
            "queued AUTH and later invalid GET wait behind in-flight HELLO");

    completeRequest(connection, executor, 0, posted);
    require(executor.requests.size() == 2 && executor.requests[1].opcode == Opcode::AUTH,
            "queued AUTH is dispatched before the later protocol error");
    completeRequest(connection, executor, 1, posted);
    require(executor.requests.size() == 2, "invalid GET never reaches the business executor");

    for (std::size_t attempt = 0; attempt < 512; ++attempt) {
        connection.onWritable(1);
    }
    const std::vector<Frame> responses = decodeFrames(transportView->writtenBytes(),
                                                       "decode responses around deferred protocol error");
    require(responses.size() == 3, "HELLO, AUTH, and deferred protocol error each produce a response");
    require(responses[0].header.requestId == 410 &&
                (responses[0].header.flags & RESPONSE) != 0 &&
                (responses[0].header.flags & ERROR) == 0,
            "HELLO response precedes queued AUTH");
    require(responses[1].header.requestId == 411 &&
                (responses[1].header.flags & RESPONSE) != 0 &&
                (responses[1].header.flags & ERROR) == 0,
            "queued AUTH response precedes protocol error");
    require(responses[2].header.requestId == 412 &&
                (responses[2].header.flags & RESPONSE) != 0 &&
                (responses[2].header.flags & ERROR) != 0,
            "protocol error response retains the invalid request identifier");
    require(connection.getState() == ConnectionState::CLOSED && transportView->isClosed(),
            "protocol error closes the connection after queued responses drain");
}

/*
    功能：验证对端半关闭后已接收请求及其响应仍按序排空
    传参：无
    返回值：无
*/
void testConnectionDrainsRequestsAfterPeerHalfClose() {
    ByteBuffer input = encodeFrame(makeHelloFrame(420), "encode half-closed pipelined HELLO");
    const ByteBuffer authBytes = encodeFrame(makeAuthFrame(421), "encode half-closed pipelined AUTH");
    const ByteBuffer getBytes = encodeFrame(
        makeFrame(Opcode::GET, 422, {makeTextBytesField(FieldId::KEY, FieldType::BYTES, "key")}),
        "encode half-closed pipelined GET");
    input.insert(input.end(), authBytes.begin(), authBytes.end());
    input.insert(input.end(), getBytes.begin(), getBytes.end());

    auto transport = std::make_unique<MemoryTransport>(std::move(input), 5, 3, true);
    MemoryTransport* transportView = transport.get();
    RecordingExecutor executor;
    std::vector<CommandResponse> posted;
    Connection connection(55, 1, std::move(transport), 4, Endpoint{"127.0.0.1", 1234},
                         DEFAULT_MAX_FRAME_BYTES, 7, {},
                         [&posted](CommandResponse response) { posted.push_back(std::move(response)); });

    connection.onReadable(executor, 4096);
    require(executor.requests.size() == 1 && executor.requests.front().opcode == Opcode::HELLO,
            "half-close preserves the in-flight HELLO");
    completeRequest(connection, executor, 0, posted);
    require(executor.requests.size() == 2 && executor.requests[1].opcode == Opcode::AUTH,
            "half-close drains queued AUTH after HELLO");
    completeRequest(connection, executor, 1, posted);
    require(executor.requests.size() == 3 && executor.requests[2].opcode == Opcode::GET,
            "half-close drains queued GET after AUTH");
    completeRequest(connection, executor, 2, posted);

    for (std::size_t attempt = 0; attempt < 512; ++attempt) {
        connection.onWritable(1);
    }
    const std::vector<Frame> responses = decodeFrames(transportView->writtenBytes(),
                                                       "decode responses after peer half-close");
    require(responses.size() == 3, "half-closed connection writes every queued response");
    for (std::size_t index = 0; index < responses.size(); ++index) {
        require(responses[index].header.requestId == 420 + index,
                "half-close responses preserve request order");
        require((responses[index].header.flags & RESPONSE) != 0 &&
                    (responses[index].header.flags & ERROR) == 0,
                "half-close requests complete successfully");
    }
    require(connection.getState() == ConnectionState::CLOSED && transportView->isClosed(),
            "half-closed connection closes after writing queued responses");
}

/*
    功能：验证损坏帧关闭连接且不会派发业务请求
    传参：无
    返回值：无
*/
void testConnectionRejectsInvalidFrame() {
    ByteBuffer invalid = encodeFrame(makeFrame(Opcode::PING, 200), "encode malformed-frame fixture");
    invalid[0] = static_cast<std::uint8_t>('X');
    auto transport = std::make_unique<MemoryTransport>(std::move(invalid));
    MemoryTransport* transportView = transport.get();
    RecordingExecutor executor;
    Connection connection(52, 1, std::move(transport), 2);
    connection.onReadable(executor, 1024);
    require(connection.getState() == ConnectionState::CLOSED && transportView->isClosed(),
            "invalid wire frame closes connection");
    require(executor.requests.empty(), "invalid frame never reaches command executor");
}

#if defined(__linux__) && !defined(_WIN32)

/*
    地位：网络服务端集成测试用命令执行器
    功能：立即返回成功结果，不依赖存储层或慢速业务工作线程。
*/
class ImmediateExecutor final : public ICommandExecutor {
public:
    /*
        功能：立即完成已提交的网络命令
        传参：request：结构化命令；completion：网络层完成回调
        返回值：无
    */
    void execute(CommandRequest request, CommandCompletion completion) override {
        CommandResponse response;
        response.connectionId = request.connectionId;
        response.requestId = request.requestId;
        completion(std::move(response));
    }
};

/*
    功能：预留并释放一个当前可用的本机 TCP 端口
    传参：无
    返回值：内核分配的回环端口号
*/
std::uint16_t reserveLoopbackPort() {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    require(descriptor >= 0, "create ephemeral-port socket");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "bind ephemeral-port socket");
    socklen_t addressLength = sizeof(address);
    require(::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &addressLength) == 0,
            "read ephemeral port");
    const std::uint16_t port = ntohs(address.sin_port);
    ::close(descriptor);
    return port;
}

/*
    功能：完整发送一段客户端网络字节
    传参：descriptor：客户端 socket；bytes：待发送字节流
    返回值：无
*/
void sendAll(int descriptor, const ByteBuffer& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t amount = ::send(descriptor, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        require(amount > 0, "send complete client request");
        offset += static_cast<std::size_t>(amount);
    }
}

/*
    功能：从客户端 socket 精确接收指定长度
    传参：descriptor：客户端 socket；bytes：目标缓冲；length：需要接收的长度
    返回值：无
*/
void receiveExact(int descriptor, ByteBuffer& bytes, std::size_t length) {
    const std::size_t originalSize = bytes.size();
    bytes.resize(originalSize + length);
    std::size_t offset = originalSize;
    while (offset < bytes.size()) {
        const ssize_t amount = ::recv(descriptor, bytes.data() + offset, bytes.size() - offset, 0);
        require(amount > 0, "receive complete server response");
        offset += static_cast<std::size_t>(amount);
    }
}

/*
    功能：接收一个完整协议响应帧
    传参：descriptor：客户端 socket
    返回值：从服务端接收的原始完整帧
*/
ByteBuffer receiveWireFrame(int descriptor) {
    ByteBuffer bytes;
    receiveExact(descriptor, bytes, BASE_HEADER_LENGTH);
    const std::uint32_t bodyLength =
        (static_cast<std::uint32_t>(bytes[10]) << 24U) |
        (static_cast<std::uint32_t>(bytes[11]) << 16U) |
        (static_cast<std::uint32_t>(bytes[12]) << 8U) |
        static_cast<std::uint32_t>(bytes[13]);
    receiveExact(descriptor, bytes, bodyLength);
    return bytes;
}

/*
    功能：验证服务启动安全策略、epoll 唤醒和真实 TCP 请求响应
    传参：无
    返回值：无
*/
void testLinuxServerIntegration() {
    const auto executor = std::make_shared<ImmediateExecutor>();

    ServerConfig defaultConfig;
    NetworkServer defaultServer(defaultConfig, executor);
    require(!defaultServer.start() && defaultServer.getState() == ServerState::CREATED,
            "default TLS policy rejects missing certificate configuration");

    ServerConfig plaintextConfig;
    plaintextConfig.tlsRequired = false;
    NetworkServer unapprovedPlaintextServer(plaintextConfig, executor);
    require(!unapprovedPlaintextServer.start(), "reject plaintext without development permission");

    ServerConfig remotePlaintextConfig;
    remotePlaintextConfig.listenEndpoint.address = "0.0.0.0";
    remotePlaintextConfig.tlsRequired = false;
    remotePlaintextConfig.allowPlaintextForDevelopment = true;
    NetworkServer remoteServer(remotePlaintextConfig, executor);
    require(!remoteServer.start(), "reject remote plaintext without explicit remote permission");

    ServerConfig invalidCertificateConfig;
    invalidCertificateConfig.tlsCertificateFile = "missing-test-certificate.pem";
    invalidCertificateConfig.tlsPrivateKeyFile = "missing-test-private-key.pem";
    NetworkServer invalidCertificateServer(invalidCertificateConfig, executor);
    require(!invalidCertificateServer.start(), "reject unavailable TLS certificate material");
    require(!TlsServerContext::create("missing-test-certificate.pem", "missing-test-private-key.pem"),
            "TLS context rejects unavailable certificate material");

    EpollPoller poller;
    require(poller.wakeup(), "eventfd wakeup succeeds");
    PollEventItem event{};
    require(poller.wait(&event, 1, 1000) == 1 && event.connectionId == 0 &&
                (event.events & POLL_WAKE) != 0,
            "epoll returns cross-thread wake event");

    ServerConfig config;
    config.listenEndpoint = Endpoint{"127.0.0.1", reserveLoopbackPort()};
    config.reactorCount = 1;
    config.maxClients = 4;
    config.tlsRequired = false;
    config.allowPlaintextForDevelopment = true;
    NetworkServer server(config, executor);
    require(server.start() && server.getState() == ServerState::RUNNING,
            "start explicitly approved loopback plaintext server");

    const int client = ::socket(AF_INET, SOCK_STREAM, 0);
    require(client >= 0, "create loopback client socket");
    timeval timeout{};
    timeout.tv_sec = 3;
    require(::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
            "set bounded client receive timeout");
    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    serverAddress.sin_port = htons(config.listenEndpoint.port);
    require(::connect(client, reinterpret_cast<const sockaddr*>(&serverAddress), sizeof(serverAddress)) == 0,
            "connect to loopback network server");

    sendAll(client, encodeFrame(makeHelloFrame(301), "encode integration HELLO"));
    const ByteBuffer helloResponseBytes = receiveWireFrame(client);
    const std::vector<Frame> helloResponses = decodeFrames(helloResponseBytes, "decode integration HELLO response");
    require(helloResponses.size() == 1 && helloResponses.front().header.requestId == 301 &&
                (helloResponses.front().header.flags & RESPONSE) != 0,
            "server returns correlated HELLO response over TCP");

    sendAll(client, encodeFrame(makeFrame(Opcode::PING, 302), "encode integration PING"));
    const ByteBuffer pingResponseBytes = receiveWireFrame(client);
    const std::vector<Frame> pingResponses = decodeFrames(pingResponseBytes, "decode integration PING response");
    require(pingResponses.size() == 1 && pingResponses.front().header.requestId == 302 &&
                (pingResponses.front().header.flags & RESPONSE) != 0,
            "server returns correlated PING response over TCP");

    ::close(client);
    server.stop();
    require(server.getState() == ServerState::STOPPED, "stop server and all reactor threads");
}

#endif

}

/*
    功能：运行网络层正确性测试集合
    传参：无
    返回值：全部通过时返回 0
*/
int main() {
    testFrameCodec();
    testCommandParser();
    testSession();
    testConnectionOrdering();
    testConnectionRejectsQueuedRequestsAfterHelloFailure();
    testConnectionDefersProtocolErrorAfterQueuedRequest();
    testConnectionDrainsRequestsAfterPeerHalfClose();
    testConnectionRejectsInvalidFrame();
#if defined(__linux__) && !defined(_WIN32)
    testLinuxServerIntegration();
#endif
    std::cout << "All network correctness tests passed.\n";
    return 0;
}
