/*
    模块名：网络帧编解码器
    模块地位：实现协议帧的增量解析与响应编码。
    模块功能描述：增量解析 TCP 字节流中的固定头、TLV body 和完整帧，并将响应对象编码为可发送字节。
*/

#include "frame_codec.h"
#include "../common/utf8.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace mydb::net {
namespace {

constexpr std::size_t FIXED_HEADER_SIZE = 24;                                       // 固定头长度：MYDB 基础协议帧头字节数
constexpr std::uint8_t KNOWN_FLAG_MASK = RESPONSE | ERROR | COMPRESSED | MORE;      // 已知标志：协议帧头定义的全部标志位
constexpr std::uint8_t UNSUPPORTED_FLAG_MASK = COMPRESSED | MORE;                   // 暂不支持标志：当前实现拒绝的压缩与续帧标志
constexpr std::size_t TLV_HEADER_SIZE = 8;                                          // TLV 头长度：字段编号、类型、保留位和长度字段总字节数
constexpr std::size_t MAX_NESTED_DEPTH = 8;                                         // 嵌套上限：允许解析的 TLV 最大递归层数

/*
    函数：readU16
    传参：data：指向两个网络序字节的起始位置
    功能：读取并转换一个 16 位无符号整数
    返回值：转换后的主机序整数
*/
std::uint16_t readU16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) |
                                      static_cast<std::uint16_t>(data[1]));
}

/*
    函数：readU32
    传参：data：指向四个网络序字节的起始位置
    功能：读取并转换一个 32 位无符号整数
    返回值：转换后的主机序整数
*/
std::uint32_t readU32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

/*
    函数：readU64
    传参：data：指向八个网络序字节的起始位置
    功能：读取并转换一个 64 位无符号整数
    返回值：转换后的主机序整数
*/
std::uint64_t readU64(const std::uint8_t* data) {
    std::uint64_t value = 0; // 解码结果：按大端顺序累积的 64 位数值
    for (std::size_t index = 0; index < 8; ++index) { // 字节下标：遍历 64 位字段的八个字节
        value = (value << 8U) | data[index];
    }
    return value;
}

/*
    函数：appendU16
    传参：data：接收编码字节的缓冲区；value：待编码的整数
    功能：按网络字节序向缓冲区追加 16 位整数
    返回值：无
*/
void appendU16(ByteBuffer& data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    data.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

/*
    函数：appendU32
    传参：data：接收编码字节的缓冲区；value：待编码的整数
    功能：按网络字节序向缓冲区追加 32 位整数
    返回值：无
*/
void appendU32(ByteBuffer& data, std::uint32_t value) {
    data.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    data.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    data.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    data.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

/*
    函数：appendU64
    传参：data：接收编码字节的缓冲区；value：待编码的整数
    功能：按网络字节序向缓冲区追加 64 位整数
    返回值：无
*/
void appendU64(ByteBuffer& data, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) { // 位移量：按大端顺序提取 64 位值的各字节
        data.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
}

/*
    函数：isKnownFieldType
    传参：type：TLV 字段头中的原始类型编号
    功能：判断类型编号是否属于当前协议定义范围
    返回值：类型编号已定义时为 true
*/
bool isKnownFieldType(std::uint8_t type) {
    return type >= static_cast<std::uint8_t>(FieldType::BYTES) &&
           type <= static_cast<std::uint8_t>(FieldType::NESTED);
}

/*
    函数：validateTypeValue
    传参：type：字段类型；value：字段值起始位置；length：字段值字节数；nestedDepth：当前嵌套层数
    功能：校验字段值长度、文本编码或嵌套 TLV 结构
    返回值：字段值符合类型约束时为 true
*/
bool validateTypeValue(FieldType type, const std::uint8_t* value, std::size_t length,
                       std::size_t nestedDepth);

/*
    函数：validateTlvSequence
    传参：data：TLV 序列起始位置；length：序列字节数；nestedDepth：当前嵌套层数
    功能：递归校验 TLV 序列的字段头、长度和字段值
    返回值：整个序列结构有效时为 true
*/
bool validateTlvSequence(const std::uint8_t* data, std::size_t length, std::size_t nestedDepth) {
    std::size_t offset = 0; // 字节偏移：当前 TLV 字段在输入序列中的读取位置
    while (offset < length) {
        if (length - offset < TLV_HEADER_SIZE) {
            // 剩余数据不足一个 TLV 头部，当前序列无法完整解析。
            return false;
        }

        const std::uint8_t rawType = data[offset + 2]; // 原始类型：当前字段头中的 wire type 数值
        const std::uint8_t reserved = data[offset + 3]; // 保留位：必须为零的字段头字节
        const std::uint32_t valueLength = readU32(data + offset + 4); // 值长度：当前字段值的字节数
        offset += TLV_HEADER_SIZE;

        if (reserved != 0 || !isKnownFieldType(rawType) ||
            static_cast<std::size_t>(valueLength) > length - offset) {
            // 保留位、类型编号或字段长度违反协议约束，拒绝该序列。
            return false;
        }
        if (!validateTypeValue(static_cast<FieldType>(rawType), data + offset,
                               static_cast<std::size_t>(valueLength), nestedDepth)) {
            // 字段值未通过对应类型校验，拒绝该序列。
            return false;
        }
        offset += static_cast<std::size_t>(valueLength);
    }
    return offset == length;
}

bool validateTypeValue(FieldType type, const std::uint8_t* value, std::size_t length,
                       std::size_t nestedDepth) {
    switch (type) {
    case FieldType::BYTES:
        return true;
    case FieldType::UTF8:
        return isValidUtf8(value, length);
    case FieldType::U64:
    case FieldType::I64:
        return length == 8;
    case FieldType::BOOL:
        return length == 1 && value[0] <= 1;
    case FieldType::NESTED:
        if (nestedDepth >= MAX_NESTED_DEPTH) {
            // 嵌套层数达到协议上限，停止递归解析以拒绝过深结构。
            return false;
        }
        return validateTlvSequence(value, length, nestedDepth + 1);
    }
    return false;
}

/*
    函数：validateFields
    传参：fields：待编码字段列表；bodySize：输出的 TLV Body 总字节数
    功能：校验字段类型和值长度，并累计编码后的 Body 长度
    返回值：字段列表可编码且长度计算有效时为 true
*/
bool validateFields(const std::vector<TlvField>& fields, std::size_t& bodySize) {
    bodySize = 0;
    for (const TlvField& field : fields) { // field：当前待校验并计入 Body 长度的字段
        const auto rawType = static_cast<std::uint8_t>(field.type); // 原始类型：转换为协议编码使用的数值
        if (!isKnownFieldType(rawType) || field.value.size() > std::numeric_limits<std::uint32_t>::max() ||
            !validateTypeValue(field.type, field.value.data(), field.value.size(), 0)) {
            // 字段类型、长度或值不符合协议约束，终止整组字段校验。
            return false;
        }
        if (field.value.size() > std::numeric_limits<std::size_t>::max() - TLV_HEADER_SIZE ||
            bodySize > std::numeric_limits<std::size_t>::max() - TLV_HEADER_SIZE - field.value.size()) {
            // 累计字段长度会发生整数溢出，拒绝继续计算 Body 大小。
            return false;
        }
        bodySize += TLV_HEADER_SIZE + field.value.size();
    }
    return true;
}

/*
    函数：validateHeaderValues
    传参：version：帧协议版本；flags：帧标志；headerLength：帧头长度
    功能：校验固定帧头中的版本、标志组合和长度
    返回值：帧头字段符合当前协议约束时为 true
*/
bool validateHeaderValues(std::uint8_t version, std::uint8_t flags, std::uint16_t headerLength) {
    return version == PROTOCOL_VERSION && headerLength == BASE_HEADER_LENGTH &&
           (flags & static_cast<std::uint8_t>(~KNOWN_FLAG_MASK)) == 0 &&
           (flags & UNSUPPORTED_FLAG_MASK) == 0 &&
           ((flags & ERROR) == 0 || (flags & RESPONSE) != 0);
}

/*
    函数：parseBody
    传参：data：Body 起始位置；length：Body 字节数；fields：接收解析字段的输出列表
    功能：解析并校验 Body 中连续排列的 TLV 字段
    返回值：全部字段均可解析时为 true
*/
bool parseBody(const std::uint8_t* data, std::size_t length, std::vector<TlvField>& fields) {
    std::size_t offset = 0; // 字节偏移：当前 TLV 字段在 Body 中的读取位置
    while (offset < length) {
        if (length - offset < TLV_HEADER_SIZE) {
            // Body 剩余部分不足一个 TLV 头部，当前帧字段区无效。
            return false;
        }

        const std::uint16_t fieldId = readU16(data + offset);               // 字段编号：当前 TLV 的 wire field_id
        const std::uint8_t rawType = data[offset + 2];                      // 原始类型：当前字段头中的 wire type 数值
        const std::uint8_t reserved = data[offset + 3];                     // 保留位：必须为零的字段头字节
        const std::uint32_t valueLength = readU32(data + offset + 4);       // 值长度：当前字段值的字节数
        offset += TLV_HEADER_SIZE;

        if (reserved != 0 || !isKnownFieldType(rawType) ||
            static_cast<std::size_t>(valueLength) > length - offset) {
            // 字段保留位、类型编号或值长度无效，拒绝当前 Body。
            return false;
        }
        const auto type = static_cast<FieldType>(rawType); // 字段类型：转换后的内部类型枚举
        if (!validateTypeValue(type, data + offset, static_cast<std::size_t>(valueLength), 0)) {
            // 字段值与声明类型不匹配，拒绝当前 Body。
            return false;
        }

        TlvField field; // 解析字段：接收当前 TLV 的编号、类型和值
        field.fieldId = fieldId;
        field.type = type;
        field.value.assign(data + offset, data + offset + static_cast<std::size_t>(valueLength));
        fields.push_back(std::move(field));
        offset += static_cast<std::size_t>(valueLength);
    }
    return offset == length;
}

} // namespace

FrameCodec::FrameCodec(std::uint32_t maxFrameBytes)
    : maxFrameBytes(maxFrameBytes) {
}

DecodeStatus FrameCodec::feed(const ByteBuffer& data, std::size_t& offset, Frame& frame) {
    if (offset > data.size()) {
        // 消费偏移越过输入块边界，清空暂存数据并报告非法帧状态。
        reset();
        return DecodeStatus::INVALID_FRAME;
    }
    // 只追加完成当前帧所需的字节，避免一次读取多个粘连帧时保留无界输入缓冲。
    while (inputBuffer.size() < FIXED_HEADER_SIZE && offset < data.size()) {
        const std::size_t needed = FIXED_HEADER_SIZE - inputBuffer.size();          // 所需长度：补齐固定帧头还缺少的字节数
        const std::size_t available = data.size() - offset;                         // 可用长度：当前输入块尚未消费的字节数
        const std::size_t count = std::min(needed, available);                      // 复制长度：本轮加入帧头缓冲的字节数
        inputBuffer.insert(inputBuffer.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                           data.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    }
    if (inputBuffer.size() < FIXED_HEADER_SIZE) {
        // 固定帧头尚未收齐，保留已有字节并等待后续输入。
        return DecodeStatus::NEED_MORE;
    }

    const std::uint8_t* bytes = inputBuffer.data(); // 帧头指针：指向已暂存的完整固定帧头
    if (bytes[0] != 'M' || bytes[1] != 'Y' || bytes[2] != 'D' || bytes[3] != 'B') {
        // 帧魔数不匹配，清空输入缓冲并拒绝当前帧。
        reset();
        return DecodeStatus::INVALID_FRAME;
    }

    const std::uint8_t version = bytes[4];                      // 协议版本：从固定帧头提取的版本号
    const std::uint8_t flags = bytes[5];                        // 帧标志：从固定帧头提取的响应和控制标志
    const std::uint16_t opcode = readU16(bytes + 6);            // 命令码：从固定帧头提取的操作编号
    const std::uint16_t headerLength = readU16(bytes + 8);      // 帧头长度：当前协议固定头的字节数
    const std::uint32_t bodyLength = readU32(bytes + 10);       // Body 长度：后续 TLV 字段区的字节数
    const std::uint64_t requestId = readU64(bytes + 14);        // 请求标识：关联本帧请求与响应
    const std::uint16_t reserved = readU16(bytes + 22);         // 保留字段：协议规定必须为零的帧头值

    if (!validateHeaderValues(version, flags, headerLength) || reserved != 0) {
        // 帧头字段或保留值无效，清空输入缓冲并报告非法帧。
        reset();
        return DecodeStatus::INVALID_FRAME;
    }
    if (maxFrameBytes < headerLength || bodyLength > maxFrameBytes - headerLength) {
        // 声明帧长超过配置上限，清空输入缓冲并报告超大帧。
        reset();
        return DecodeStatus::FRAME_TOO_LARGE;
    }

    const std::size_t totalLength = static_cast<std::size_t>(headerLength) + bodyLength; // 帧总长度：固定头和 Body 的合计字节数
    if (inputBuffer.size() < totalLength && offset < data.size()) {
        // 当前缓冲未包含完整帧且输入块仍有数据，将必要字节追加到帧缓冲。
        const std::size_t needed = totalLength - inputBuffer.size(); // 所需长度：补齐当前完整帧还缺少的字节数
        const std::size_t available = data.size() - offset; // 可用长度：当前输入块尚未消费的字节数
        const std::size_t count = std::min(needed, available); // 复制长度：本轮加入帧缓冲的字节数
        inputBuffer.insert(inputBuffer.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                           data.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    }
    if (inputBuffer.size() < totalLength) {
        // 当前帧仍未收齐，保留已读字节并等待后续输入。
        return DecodeStatus::NEED_MORE;
    }

    Frame parsed; // 解析帧：暂存已校验的帧头和解析后的 TLV 字段
    parsed.header.version = version;
    parsed.header.flags = flags;
    parsed.header.opcode = static_cast<Opcode>(opcode);
    parsed.header.headerLength = headerLength;
    parsed.header.bodyLength = bodyLength;
    parsed.header.requestId = requestId;
    if (!parseBody(inputBuffer.data() + headerLength, bodyLength, parsed.fields)) {
        // Body 解析失败，丢弃暂存帧并返回协议错误。
        reset();
        return DecodeStatus::INVALID_FRAME;
    }

    inputBuffer.erase(inputBuffer.begin(), inputBuffer.begin() + static_cast<std::ptrdiff_t>(totalLength));
    frame = std::move(parsed);
    return DecodeStatus::FRAME_READY;
}

DecodeStatus FrameCodec::feed(const ByteBuffer& data, Frame& frame) {
    std::size_t offset = 0; // 输入偏移：记录本次调用从输入字节块消费的位置
    const DecodeStatus status = feed(data, offset, frame); // 解码状态：判断本次解析是否完成及是否存在剩余数据
    if (status != DecodeStatus::FRAME_READY || offset == data.size()) {
        // 尚未得到完整帧或输入块已全部消费，直接返回当前解码结果。
        return status;
    }

    // 兼容旧的单帧解析接口，将粘连帧的剩余字节暂存供后续调用继续解析。
    const std::size_t legacyLimit = maxFrameBytes <= std::numeric_limits<std::size_t>::max() / 2
        ? static_cast<std::size_t>(maxFrameBytes) * 2
        : std::numeric_limits<std::size_t>::max(); // 兼容缓冲上限：限制旧式单帧接口可暂存的粘连数据
    const std::size_t suffixSize = data.size() - offset; // 剩余长度：当前输入块中尚未消费的粘连帧字节
    if (suffixSize > legacyLimit || inputBuffer.size() > legacyLimit - suffixSize) {
        // 粘连数据将超过兼容接口缓冲上限，清空暂存区并报告超大帧。
        reset();
        return DecodeStatus::FRAME_TOO_LARGE;
    }
    inputBuffer.insert(inputBuffer.end(), data.begin() + static_cast<std::ptrdiff_t>(offset), data.end());
    return status;
}

ByteBuffer FrameCodec::encode(const Frame& frame) const {
    const auto opcode = static_cast<std::uint16_t>(frame.header.opcode); // 命令码：转换为帧头编码使用的数值
    if (!validateHeaderValues(frame.header.version, frame.header.flags, frame.header.headerLength)) {
        // 帧头字段不符合当前协议约束，拒绝生成编码结果。
        return {};
    }

    std::size_t bodySize = 0; // Body 长度：由字段校验过程计算的 TLV 总字节数
    if (!validateFields(frame.fields, bodySize) || bodySize > std::numeric_limits<std::uint32_t>::max() ||
        maxFrameBytes < frame.header.headerLength || bodySize > maxFrameBytes - frame.header.headerLength) {
        // 字段无效或编码后的帧长超限，拒绝生成编码结果。
        return {};
    }

    ByteBuffer encoded; // 编码帧：接收固定头和 TLV Body 的序列化字节
    encoded.reserve(frame.header.headerLength + bodySize);
    encoded.insert(encoded.end(), {'M', 'Y', 'D', 'B'});
    encoded.push_back(frame.header.version);
    encoded.push_back(frame.header.flags);
    appendU16(encoded, opcode);
    appendU16(encoded, frame.header.headerLength);
    appendU32(encoded, static_cast<std::uint32_t>(bodySize));
    appendU64(encoded, frame.header.requestId);
    appendU16(encoded, 0);

    for (const TlvField& field : frame.fields) { // field：当前依序编码到帧 Body 的字段
        appendU16(encoded, field.fieldId);
        encoded.push_back(static_cast<std::uint8_t>(field.type));
        encoded.push_back(0);
        appendU32(encoded, static_cast<std::uint32_t>(field.value.size()));
        encoded.insert(encoded.end(), field.value.begin(), field.value.end());
    }
    return encoded;
}

void FrameCodec::reset() {
    inputBuffer.clear();
}

} // namespace mydb::net
