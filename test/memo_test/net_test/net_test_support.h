#pragma once
#ifndef _MYDB_NET_TEST_SUPPORT_H_
#define _MYDB_NET_TEST_SUPPORT_H_

/*
    模块名：网络层测试公共夹具
    模块地位：为正确性测试和压力测试提供协议构造、断言及可控传输工具
    模块功能描述：构造确定性网络帧，并以内存传输和可记录执行器隔离外部存储依赖。
*/

#include "net_level/command/command_executor.h"
#include "net_level/command/protocol_fields.h"
#include "net_level/protocol/frame_codec.h"
#include "net_level/transport/transport.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace mydb::net::test {

/*
    功能：条件不成立时打印用例名并以失败状态退出
    传参：condition：测试条件；name：用例名称
    返回值：无
*/
inline void require(bool condition, std::string_view name) {
    if (!condition) {
        std::cerr << "[FAIL] " << name << '\n';
        std::exit(EXIT_FAILURE);
    }
}

/*
    功能：构造协议字段
    传参：id：字段编号；type：字段类型；value：字段原始值
    返回值：构造完成的字段
*/
inline TlvField makeField(std::uint16_t id, FieldType type, ByteBuffer value) {
    TlvField field;
    field.fieldId = id;
    field.type = type;
    field.value = std::move(value);
    return field;
}

/*
    功能：构造包含文本或二进制内容的协议字段
    传参：id：字段编号；type：字段类型；value：原始内容
    返回值：构造完成的字段
*/
inline TlvField makeTextBytesField(FieldId id, FieldType type, std::string_view value) {
    return makeField(fieldId(id), type, ByteBuffer(value.begin(), value.end()));
}

/*
    功能：构造网络字节序的无符号 64 位字段
    传参：id：字段编号；value：字段整数值
    返回值：构造完成的字段
*/
inline TlvField makeU64Field(FieldId id, std::uint64_t value) {
    ByteBuffer bytes;
    bytes.reserve(8);
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
    return makeField(fieldId(id), FieldType::U64, std::move(bytes));
}

/*
    功能：构造字段长度与 Body 长度匹配的请求帧
    传参：opcode：命令码；requestId：请求关联标识；fields：请求字段
    返回值：构造完成的协议帧
*/
inline Frame makeFrame(Opcode opcode, RequestId requestId, std::vector<TlvField> fields = {}) {
    Frame frame;
    frame.header.version = static_cast<std::uint8_t>(PROTOCOL_VERSION);
    frame.header.flags = 0;
    frame.header.opcode = opcode;
    frame.header.headerLength = BASE_HEADER_LENGTH;
    frame.header.requestId = requestId;
    frame.fields = std::move(fields);
    std::size_t bodyLength = 0;
    for (const TlvField& field : frame.fields) {
        bodyLength += 8U + field.value.size();
    }
    frame.header.bodyLength = static_cast<std::uint32_t>(bodyLength);
    return frame;
}

/*
    功能：编码测试帧并检查编码成功
    传参：frame：待编码的帧；name：失败时打印的用例名称
    返回值：编码后的网络字节流
*/
inline ByteBuffer encodeFrame(const Frame& frame, std::string_view name) {
    const ByteBuffer encoded = FrameCodec{}.encode(frame);
    require(!encoded.empty(), name);
    return encoded;
}

/*
    地位：网络层 Connection 的确定性传输替身
    功能：按配置的读写块大小提供内存输入输出，并可模拟对端关闭。
*/
class MemoryTransport final : public Transport {
private:
    ByteBuffer input;                              // 输入字节：测试提供的客户端请求流
    std::size_t inputOffset = 0;                   // 输入位置：已交付给 Connection 的字节数
    ByteBuffer output;                             // 输出字节：Connection 已发送的响应流
    std::size_t maxReadChunk;                      // 读块上限：模拟 TCP 任意分段
    std::size_t maxWriteChunk;                     // 写块上限：模拟短写
    bool reportPeerClosed;                         // 对端状态：输入耗尽后是否报告半关闭
    bool closed = false;                           // 本地状态：传输是否已经关闭
    std::uint8_t desiredEvents = WANT_READ;         // 事件需求：fake transport 当前关注的 I/O 方向

public:
    /*
        功能：创建内存传输
        传参：input：输入字节流；maxReadChunk：单次读取上限；maxWriteChunk：单次写入上限；peerClosed：是否模拟对端关闭
        返回值：无
    */
    explicit MemoryTransport(ByteBuffer input = {},
                             std::size_t maxReadChunk = std::numeric_limits<std::size_t>::max(),
                             std::size_t maxWriteChunk = std::numeric_limits<std::size_t>::max(),
                             bool peerClosed = false)
        : input(std::move(input)), maxReadChunk(maxReadChunk), maxWriteChunk(maxWriteChunk),
          reportPeerClosed(peerClosed) {}

    /*
        功能：返回测试用原生句柄
        传参：无
        返回值：固定的非负占位句柄
    */
    std::intptr_t nativeHandle() const noexcept override {
        return 1;
    }

    /*
        功能：完成内存传输握手
        传参：无
        返回值：握手成功
    */
    TransportResult handshake() override {
        desiredEvents = WANT_READ;
        return TransportResult::OK;
    }

    /*
        功能：按读取预算向目标缓冲追加输入数据
        传参：buffer：接收缓冲；maxBytes：本次读取预算
        返回值：读取进展、暂时无数据或对端关闭
    */
    TransportResult read(ByteBuffer& buffer, std::size_t maxBytes) override {
        if (inputOffset == input.size()) {
            return reportPeerClosed ? TransportResult::PEER_CLOSED : TransportResult::WOULD_BLOCK;
        }
        const std::size_t amount = std::min({maxBytes, maxReadChunk, input.size() - inputOffset});
        buffer.insert(buffer.end(), input.begin() + static_cast<std::ptrdiff_t>(inputOffset),
                      input.begin() + static_cast<std::ptrdiff_t>(inputOffset + amount));
        inputOffset += amount;
        return TransportResult::OK;
    }

    /*
        功能：按写入块上限收集 Connection 输出
        传参：buffer：待发送字节；offset：已发送位置
        返回值：写入成功
    */
    TransportResult write(std::span<const std::uint8_t> buffer, std::size_t& offset) override {
        const std::size_t amount = std::min(maxWriteChunk, buffer.size() - offset);
        output.insert(output.end(), buffer.begin() + static_cast<std::ptrdiff_t>(offset),
                      buffer.begin() + static_cast<std::ptrdiff_t>(offset + amount));
        offset += amount;
        desiredEvents = WANT_READ;
        return TransportResult::OK;
    }

    /*
        功能：返回内存传输的当前事件需求
        传参：无
        返回值：读事件位
    */
    std::uint8_t events() const override {
        return desiredEvents;
    }

    /*
        功能：标记内存传输已关闭
        传参：无
        返回值：无
    */
    void close() override {
        closed = true;
    }

    /*
        功能：读取已收集的响应字节
        传参：无
        返回值：只读输出缓冲
    */
    const ByteBuffer& writtenBytes() const {
        return output;
    }

    /*
        功能：读取传输关闭状态
        传参：无
        返回值：是否已关闭
    */
    bool isClosed() const {
        return closed;
    }
};

/*
    地位：网络层异步执行边界的确定性替身
    功能：记录提交的结构化请求和完成回调，供测试逐步控制业务完成顺序。
*/
class RecordingExecutor final : public ICommandExecutor {
public:
    std::vector<CommandRequest> requests;          // 请求记录：按提交顺序保存结构化请求
    std::vector<CommandCompletion> completions;     // 完成回调：由用例显式触发异步完成

    /*
        功能：保存网络层提交的请求及完成回调
        传参：request：结构化命令；completion：异步完成回调
        返回值：无
    */
    void execute(CommandRequest request, CommandCompletion completion) override {
        requests.push_back(std::move(request));
        completions.push_back(std::move(completion));
    }
};

}

#endif
