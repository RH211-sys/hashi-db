/*
	模块名：网络协议与连接核心测试
	模块地位：网络层协议行为的独立测试入口，不依赖存储层编译产物，也不依赖 Linux 运行时
	模块功能描述：用固定输入数据验证「帧编解码」和「命令解析」的对外行为，
	              每个用例都写明测试目的、输入、过程与预期结果，供不看代码的人直接阅读。
*/

#include "net_level/command/command_parser.h"
#include "net_level/protocol/frame_codec.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace mydb::net;

/*
	被测数据格式（协议 v1）

	输入和输出都是一个完整的「帧」，由固定头和若干字段组成。

	固定头（24 字节，线上按网络字节序排列）：
		1. 魔数        4 字节   固定为 MYDB，用来快速识别协议边界
		2. 协议版本    1 字节   当前为 1
		3. 标志位      1 字节   标记该帧是请求还是响应、是否为错误帧
		4. 命令码      2 字节   该帧对应的操作
		5. 头长度      2 字节   固定头的长度，当前固定为 24
		6. Body 长度   4 字节   后面所有字段一共占用的字节数
		7. 请求标识    8 字节   由客户端指定，响应必须原样带回
		8. 保留位      2 字节   当前必须为 0

	字段（TLV 形式，可以出现多次）：
		字段号（2 字节）+ 类型（1 字节）+ 保留（1 字节）+ 值长度（4 字节）+ 值（变长）

	字段类型取值：
		1=二进制、2=文本（UTF-8）、3=无符号整数、4=有符号整数、5=布尔、6=嵌套字段

	本测试用到的命令码：
		0x0001 = HELLO（协商）、0x0003 = PING（探活）

	本测试用到的字段号：
		1 = 协议版本、3 = 客户端名称、6 = 探活负载
*/

namespace {

/*
	函数：require
	参数：condition：判断条件；name：用例名称
	功能：条件不成立时打印失败的用例名并立即结束测试
	返回：无
*/
void require(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "[FAIL] " << name << '\n';
        std::exit(1);
    }
}

/*
	函数：bytesField
	参数：id：字段号；type：字段类型；value：字段的原始字节内容
	功能：构造一个用于测试的协议字段
	返回：构造好的字段
*/
TlvField bytesField(std::uint16_t id, FieldType type, std::string value) {
    TlvField field;
    field.fieldId = id;
    field.type = type;
    field.value.assign(value.begin(), value.end());
    return field;
}

/*
	函数：pingFrame
	参数：requestId：请求标识；payload：探活负载，为空表示不携带负载
	功能：构造一个探活请求帧，作为后续编码与解析用例的输入
	返回：构造好的帧
*/
Frame pingFrame(std::uint64_t requestId, std::string payload) {
    Frame frame;
    frame.header.version = static_cast<std::uint8_t>(PROTOCOL_VERSION);
    frame.header.flags = 0;
    frame.header.opcode = Opcode::PING;
    frame.header.headerLength = BASE_HEADER_LENGTH;
    frame.header.requestId = requestId;
    if (!payload.empty()) {
        frame.fields.push_back(bytesField(6, FieldType::BYTES, std::move(payload)));
        frame.header.bodyLength = static_cast<std::uint32_t>(8 + frame.fields.front().value.size());
    }
    return frame;
}

}

/*
	测试总体流程

	1. 构造若干「请求帧」作为输入，既有合法帧，也有刻意损坏的帧；
	2. TCP 上的字节流可能被拆开或粘连，因此分别验证三种到达方式：
	   逐字节到达、被切成两块到达、多帧一次性粘连到达；
	3. 校验编码与解码结果：状态取值、还原出的帧头信息、字段内容是否与输入一致；
	4. 校验非法输入被拒绝，并且给出约定的错误码。

	结果判定方式

	- 每个用例有唯一名称；条件不满足时立刻打印 [FAIL] 加用例名，并以失败状态结束测试；
	- 全部用例通过时打印一行通过提示，进程返回 0，供构建/测试脚本判定。

	三类被测结果的含义

	- 编码结果：非空字节流表示编码成功；空结果表示该帧不合法，拒绝输出；
	- 解码状态：需要更多数据 / 帧完整 / 帧非法 / 帧过大；
	- 解析结果：成功时给出结构化请求（连接标识、请求标识、命令码、字段列表），
	            失败时给出错误码和一句可读的说明。

	本测试涉及的错误码含义

	- 100 坏帧：帧头、长度或字段格式不符合协议约定；
	- 101 不支持：版本或特性当前不支持；
	- 400 未知命令：命令码不在协议定义范围内。
*/
int main() {
    /*
        用例一：二进制负载编码
        目的：确认合法请求帧能被编码成可发送的字节流，且二进制内容不被文本规则干扰
        输入：探活请求，请求标识 77，负载是含有 0x00 的三字节二进制数据
        预期：编码结果非空
    */
    FrameCodec encoder;
    const ByteBuffer encoded = encoder.encode(pingFrame(77, std::string("a\0b", 3)));
    require(!encoded.empty(), "encode binary payload");

    /*
        用例二：逐字节增量解码
        目的：确认字节流被拆成很小片段到达时，能先等待、再拼出完整帧
        过程：把用例一产生的字节流一个字节一个字节地送入解码器
        预期：最后一个字节之前都返回「需要更多数据」；
              送入最后一个字节后返回「帧完整」，还原出的请求标识为 77、
              只有 1 个字段、字段值长度为 3 且中间那个字节为 0x00
    */
    FrameCodec decoder;
    Frame frame;
    for (std::size_t index = 0; index + 1 < encoded.size(); ++index) {
        const ByteBuffer piece{encoded[index]};
        require(decoder.feed(piece, frame) == DecodeStatus::NEED_MORE, "incremental partial frame");
    }
    const ByteBuffer last{encoded.back()};
    require(decoder.feed(last, frame) == DecodeStatus::FRAME_READY, "complete final byte");
    require(frame.header.requestId == 77 && frame.fields.size() == 1, "decoded frame metadata");
    require(frame.fields.front().value.size() == 3 && frame.fields.front().value[1] == 0, "binary safe value");

    /*
        用例三：同一帧被切成两块到达
        目的：确认解析进度能跨数据块延续，不会因为换块而丢掉半帧
        过程：把完整帧从中间切成前后两块，先送前半块，再从头计数送后半块
        预期：前半块返回「需要更多数据」且该块被完整消费；
              后半块返回「帧完整」且该块被完整消费，请求标识仍为 77
    */
    FrameCodec offsetFragmentDecoder;
    const std::size_t split = encoded.size() / 2;
    ByteBuffer firstHalf(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(split));
    std::size_t fragmentOffset = 0;
    require(offsetFragmentDecoder.feed(firstHalf, fragmentOffset, frame) == DecodeStatus::NEED_MORE,
            "offset API accepts first partial chunk");
    require(fragmentOffset == firstHalf.size(), "offset API consumes first partial chunk");
    ByteBuffer secondHalf(encoded.begin() + static_cast<std::ptrdiff_t>(split), encoded.end());
    fragmentOffset = 0;
    require(offsetFragmentDecoder.feed(secondHalf, fragmentOffset, frame) == DecodeStatus::FRAME_READY,
            "offset API completes frame across chunks");
    require(frame.header.requestId == 77 && fragmentOffset == secondHalf.size(),
            "fragmented offset frame metadata");

    /*
        用例四：多帧粘连
        目的：确认一次收到多帧时能逐帧切开，且不局限于一帧大小
        输入：把请求标识 77、78、79 的三帧首尾相连成一段字节流
        过程：用同一个解码器依次取出三帧，每次都在同一段字节流上向后取
        预期：三次都返回「帧完整」，请求标识依次为 77、78、79，
              每取出一帧后位置正好落在该帧末尾；再送空数据返回「需要更多数据」
    */
    const ByteBuffer encodedSecond = encoder.encode(pingFrame(78, "second"));
    const ByteBuffer encodedThird = encoder.encode(pingFrame(79, "third"));
    ByteBuffer pair = encoded;
    pair.insert(pair.end(), encodedSecond.begin(), encodedSecond.end());
    pair.insert(pair.end(), encodedThird.begin(), encodedThird.end());
    FrameCodec coalescedDecoder;
    std::size_t pairOffset = 0;
    require(coalescedDecoder.feed(pair, pairOffset, frame) == DecodeStatus::FRAME_READY,
            "first coalesced frame");
    require(frame.header.requestId == 77 && pairOffset == encoded.size(), "first frame boundary");
    require(coalescedDecoder.feed(pair, pairOffset, frame) == DecodeStatus::FRAME_READY,
            "second coalesced frame");
    require(frame.header.requestId == 78 && pairOffset == encoded.size() + encodedSecond.size(),
            "second frame boundary");
    require(coalescedDecoder.feed(pair, pairOffset, frame) == DecodeStatus::FRAME_READY,
            "third coalesced frame beyond legacy aggregate cap");
    require(frame.header.requestId == 79 && pairOffset == pair.size(), "third frame boundary");
    require(coalescedDecoder.feed({}, frame) == DecodeStatus::NEED_MORE, "empty decoder buffer");

    /*
        用例五：魔数被损坏
        目的：确认不属于本协议的字节流被立即拒绝，而不是被当成残缺帧一直等待
        输入：在合法帧基础上把第 1 个字节改成 X
        预期：返回「帧非法」
    */
    FrameCodec invalidDecoder;
    ByteBuffer invalid = encoded;
    invalid[0] = 'X';
    require(invalidDecoder.feed(invalid, frame) == DecodeStatus::INVALID_FRAME, "reject bad magic");

    /*
        用例六：协商请求解析（HELLO）
        目的：确认合法的协商请求能被解析成结构化请求
        输入：HELLO 帧，携带协议版本字段（值为 0 和 1，即主版本 0、次版本 1），连接标识为 5
        预期：解析成功，且结果中的连接标识为 5、命令码为协商
    */
    Frame hello;
    hello.header.version = static_cast<std::uint8_t>(PROTOCOL_VERSION);
    hello.header.opcode = Opcode::HELLO;
    hello.header.headerLength = BASE_HEADER_LENGTH;
    TlvField version;
    version.fieldId = 1;
    version.type = FieldType::BYTES;
    version.value = {0, static_cast<std::uint8_t>(PROTOCOL_VERSION)};
    hello.fields.push_back(version);
    hello.header.bodyLength = 10;

    CommandParser parser;
    CommandRequest request;
    ErrorCode error = ErrorCode::INTERNAL;
    std::string message;
    require(parser.parse(hello, 5, request, error, message), "parse HELLO");
    require(request.connectionId == 5 && request.opcode == Opcode::HELLO, "parsed request context");

    /*
        用例七：客户端名称使用非 ASCII 文本
        目的：确认合法的中文等文本字段被接受
        输入：在上一帧基础上追加客户端名称字段，内容是中文「数据库」
        预期：解析成功
    */
    Frame validNameHello = hello;
    const TlvField clientName = bytesField(3, FieldType::UTF8, "数据库");
    validNameHello.fields.push_back(clientName);
    validNameHello.header.bodyLength = static_cast<std::uint32_t>(18 + clientName.value.size());
    require(parser.parse(validNameHello, 5, request, error, message), "parser accepts valid non-ASCII UTF8");

    /*
        用例八：同一字段重复出现
        目的：确认一个帧里同一字段出现两次时按坏帧拒绝，避免二义性
        输入：HELLO 帧中出现两个协议版本字段
        预期：解析失败，错误码为「坏帧」（100）
    */
    hello.fields.push_back(version);
    hello.header.bodyLength = 20;
    require(!parser.parse(hello, 5, request, error, message), "reject duplicate fields");
    require(error == ErrorCode::BAD_FRAME, "duplicate field error mapping");

    /*
        用例九：命令码不在协议定义内
        目的：确认未知命令被拒绝，并且给出可与坏帧区分的错误码
        输入：探活帧，命令码改成协议未定义的 0x7FFF，不带任何字段
        预期：解析失败，错误码为「未知命令」（400）
    */
    Frame unknown = pingFrame(78, {});
    unknown.header.opcode = static_cast<Opcode>(0x7fff);
    unknown.header.bodyLength = 0;
    require(!parser.parse(unknown, 5, request, error, message), "reject unknown opcode");
    require(error == ErrorCode::UNKNOWN_COMMAND, "unknown opcode error mapping");

    /*
        用例十：编码阶段拒绝非法文本
        目的：确认服务端不会把非法 UTF-8 文本写回给客户端
        输入：探活帧，负载字段声明为文本类型，内容是一段非法 UTF-8 字节序列
        预期：编码结果为空，即拒绝输出该帧
    */
    Frame text = pingFrame(80, std::string("\xc0\xaf", 2));
    text.fields.clear();
    text.fields.push_back(bytesField(6, FieldType::UTF8, std::string("\xc0\xaf", 2)));
    text.header.bodyLength = 10;
    FrameCodec textEncoder;
    require(textEncoder.encode(text).empty(), "encoder rejects invalid utf8");

    /*
        用例十一：解析阶段拒绝非法文本
        目的：确认非法 UTF-8 不会进入业务执行层
        输入：HELLO 帧，协议版本字段合法，客户端名称字段是非法 UTF-8 字节序列
        预期：解析失败，错误码为「坏帧」（100）
    */
    Frame malformedHello;
    malformedHello.header.version = static_cast<std::uint8_t>(PROTOCOL_VERSION);
    malformedHello.header.opcode = Opcode::HELLO;
    malformedHello.header.headerLength = BASE_HEADER_LENGTH;
    malformedHello.header.bodyLength = 20;
    malformedHello.fields.push_back(bytesField(1, FieldType::BYTES, std::string("\0\1", 2)));
    malformedHello.fields.push_back(bytesField(3, FieldType::UTF8, std::string("\xc0\xaf", 2)));
    require(!parser.parse(malformedHello, 5, request, error, message), "parser rejects malformed UTF8");
    require(error == ErrorCode::BAD_FRAME, "malformed UTF8 maps to bad frame");

    /*
        用例十二：解码阶段拒绝非法文本
        目的：确认从线上收到的非法 UTF-8 在拆包阶段就被拦下，不进入解析环节
        过程：先用合法文本「é」编码出一个帧，再把该帧里字段值的头两个字节改成非法 UTF-8 序列
        预期：合法帧编码成功；被篡改的字节流在解码时返回「帧非法」
    */
    Frame validText = pingFrame(81, std::string("é", 2));
    validText.fields.clear();
    validText.fields.push_back(bytesField(6, FieldType::UTF8, std::string("é", 2)));
    validText.header.bodyLength = 10;
    const ByteBuffer validTextWire = textEncoder.encode(validText);
    require(!validTextWire.empty(), "encoder accepts valid utf8");
    ByteBuffer invalidUtf8Wire = validTextWire;
    invalidUtf8Wire[32] = 0xc0;
    invalidUtf8Wire[33] = 0xaf;
    FrameCodec inboundTextDecoder;
    require(inboundTextDecoder.feed(invalidUtf8Wire, frame) == DecodeStatus::INVALID_FRAME,
            "decoder rejects malformed UTF8");

    /*
        测试收尾：全部用例通过后输出固定成功提示，进程返回 0；
        任何一条用例失败都会在失败处提前打印 [FAIL] 用例名并返回非 0。
    */
    std::cout << "Network codec tests passed\n";
    return 0;
}
