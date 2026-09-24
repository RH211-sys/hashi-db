/*
    模块名：网络帧编解码器
    功能描述：增量解析 TCP 字节流中的固定头、TLV body 和完整帧，并将响应对象编码为可发送字节。
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

constexpr std::size_t FIXED_HEADER_SIZE = 24;
constexpr std::uint8_t KNOWN_FLAG_MASK = RESPONSE | ERROR | COMPRESSED | MORE;
constexpr std::uint8_t UNSUPPORTED_FLAG_MASK = COMPRESSED | MORE;
constexpr std::size_t TLV_HEADER_SIZE = 8;
constexpr std::size_t MAX_NESTED_DEPTH = 8;

std::uint16_t readU16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) |
                                      static_cast<std::uint16_t>(data[1]));
}

std::uint32_t readU32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

std::uint64_t readU64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | data[index];
    }
    return value;
}

void appendU16(ByteBuffer& data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    data.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void appendU32(ByteBuffer& data, std::uint32_t value) {
    data.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    data.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    data.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    data.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void appendU64(ByteBuffer& data, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
}

bool isKnownFieldType(std::uint8_t type) {
    return type >= static_cast<std::uint8_t>(FieldType::BYTES) &&
           type <= static_cast<std::uint8_t>(FieldType::NESTED);
}

bool validateTypeValue(FieldType type, const std::uint8_t* value, std::size_t length,
                       std::size_t nestedDepth);

bool validateTlvSequence(const std::uint8_t* data, std::size_t length, std::size_t nestedDepth) {
    std::size_t offset = 0;
    while (offset < length) {
        if (length - offset < TLV_HEADER_SIZE) {
            return false;
        }

        const std::uint8_t rawType = data[offset + 2];
        const std::uint8_t reserved = data[offset + 3];
        const std::uint32_t valueLength = readU32(data + offset + 4);
        offset += TLV_HEADER_SIZE;

        if (reserved != 0 || !isKnownFieldType(rawType) ||
            static_cast<std::size_t>(valueLength) > length - offset) {
            return false;
        }
        if (!validateTypeValue(static_cast<FieldType>(rawType), data + offset,
                               static_cast<std::size_t>(valueLength), nestedDepth)) {
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
            return false;
        }
        return validateTlvSequence(value, length, nestedDepth + 1);
    }
    return false;
}

bool validateFields(const std::vector<TlvField>& fields, std::size_t& bodySize) {
    bodySize = 0;
    for (const TlvField& field : fields) {
        const auto rawType = static_cast<std::uint8_t>(field.type);
        if (!isKnownFieldType(rawType) || field.value.size() > std::numeric_limits<std::uint32_t>::max() ||
            !validateTypeValue(field.type, field.value.data(), field.value.size(), 0)) {
            return false;
        }
        if (field.value.size() > std::numeric_limits<std::size_t>::max() - TLV_HEADER_SIZE ||
            bodySize > std::numeric_limits<std::size_t>::max() - TLV_HEADER_SIZE - field.value.size()) {
            return false;
        }
        bodySize += TLV_HEADER_SIZE + field.value.size();
    }
    return true;
}

bool validateHeaderValues(std::uint8_t version, std::uint8_t flags, std::uint16_t headerLength) {
    return version == PROTOCOL_VERSION && headerLength == BASE_HEADER_LENGTH &&
           (flags & static_cast<std::uint8_t>(~KNOWN_FLAG_MASK)) == 0 &&
           (flags & UNSUPPORTED_FLAG_MASK) == 0 &&
           ((flags & ERROR) == 0 || (flags & RESPONSE) != 0);
}

bool parseBody(const std::uint8_t* data, std::size_t length, std::vector<TlvField>& fields) {
    std::size_t offset = 0;
    while (offset < length) {
        if (length - offset < TLV_HEADER_SIZE) {
            return false;
        }

        const std::uint16_t fieldId = readU16(data + offset);
        const std::uint8_t rawType = data[offset + 2];
        const std::uint8_t reserved = data[offset + 3];
        const std::uint32_t valueLength = readU32(data + offset + 4);
        offset += TLV_HEADER_SIZE;

        if (reserved != 0 || !isKnownFieldType(rawType) ||
            static_cast<std::size_t>(valueLength) > length - offset) {
            return false;
        }
        const auto type = static_cast<FieldType>(rawType);
        if (!validateTypeValue(type, data + offset, static_cast<std::size_t>(valueLength), 0)) {
            return false;
        }

        TlvField field;
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
        reset();
        return DecodeStatus::INVALID_FRAME;
    }
    // Append only bytes needed to finish this frame so a large coalesced read does
    // not become an unbounded retained buffer.
    while (inputBuffer.size() < FIXED_HEADER_SIZE && offset < data.size()) {
        const std::size_t needed = FIXED_HEADER_SIZE - inputBuffer.size();
        const std::size_t available = data.size() - offset;
        const std::size_t count = std::min(needed, available);
        inputBuffer.insert(inputBuffer.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                           data.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    }
    if (inputBuffer.size() < FIXED_HEADER_SIZE) {
        return DecodeStatus::NEED_MORE;
    }

    const std::uint8_t* bytes = inputBuffer.data();
    if (bytes[0] != 'M' || bytes[1] != 'Y' || bytes[2] != 'D' || bytes[3] != 'B') {
        reset();
        return DecodeStatus::INVALID_FRAME;
    }

    const std::uint8_t version = bytes[4];
    const std::uint8_t flags = bytes[5];
    const std::uint16_t opcode = readU16(bytes + 6);
    const std::uint16_t headerLength = readU16(bytes + 8);
    const std::uint32_t bodyLength = readU32(bytes + 10);
    const std::uint64_t requestId = readU64(bytes + 14);
    const std::uint16_t reserved = readU16(bytes + 22);

    if (!validateHeaderValues(version, flags, headerLength) || reserved != 0) {
        reset();
        return DecodeStatus::INVALID_FRAME;
    }
    if (maxFrameBytes < headerLength || bodyLength > maxFrameBytes - headerLength) {
        reset();
        return DecodeStatus::FRAME_TOO_LARGE;
    }

    const std::size_t totalLength = static_cast<std::size_t>(headerLength) + bodyLength;
    if (inputBuffer.size() < totalLength && offset < data.size()) {
        const std::size_t needed = totalLength - inputBuffer.size();
        const std::size_t available = data.size() - offset;
        const std::size_t count = std::min(needed, available);
        inputBuffer.insert(inputBuffer.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                           data.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    }
    if (inputBuffer.size() < totalLength) {
        return DecodeStatus::NEED_MORE;
    }

    Frame parsed;
    parsed.header.version = version;
    parsed.header.flags = flags;
    parsed.header.opcode = static_cast<Opcode>(opcode);
    parsed.header.headerLength = headerLength;
    parsed.header.bodyLength = bodyLength;
    parsed.header.requestId = requestId;
    if (!parseBody(inputBuffer.data() + headerLength, bodyLength, parsed.fields)) {
        reset();
        return DecodeStatus::INVALID_FRAME;
    }

    inputBuffer.erase(inputBuffer.begin(), inputBuffer.begin() + static_cast<std::ptrdiff_t>(totalLength));
    frame = std::move(parsed);
    return DecodeStatus::FRAME_READY;
}

DecodeStatus FrameCodec::feed(const ByteBuffer& data, Frame& frame) {
    std::size_t offset = 0;
    const DecodeStatus status = feed(data, offset, frame);
    if (status != DecodeStatus::FRAME_READY || offset == data.size()) {
        return status;
    }

    // Legacy one-frame API buffers coalesced suffixes for later empty-feed calls.
    const std::size_t legacyLimit = maxFrameBytes <= std::numeric_limits<std::size_t>::max() / 2
        ? static_cast<std::size_t>(maxFrameBytes) * 2
        : std::numeric_limits<std::size_t>::max();
    const std::size_t suffixSize = data.size() - offset;
    if (suffixSize > legacyLimit || inputBuffer.size() > legacyLimit - suffixSize) {
        reset();
        return DecodeStatus::FRAME_TOO_LARGE;
    }
    inputBuffer.insert(inputBuffer.end(), data.begin() + static_cast<std::ptrdiff_t>(offset), data.end());
    return status;
}

ByteBuffer FrameCodec::encode(const Frame& frame) const {
    const auto opcode = static_cast<std::uint16_t>(frame.header.opcode);
    if (!validateHeaderValues(frame.header.version, frame.header.flags, frame.header.headerLength)) {
        return {};
    }

    std::size_t bodySize = 0;
    if (!validateFields(frame.fields, bodySize) || bodySize > std::numeric_limits<std::uint32_t>::max() ||
        maxFrameBytes < frame.header.headerLength || bodySize > maxFrameBytes - frame.header.headerLength) {
        return {};
    }

    ByteBuffer encoded;
    encoded.reserve(frame.header.headerLength + bodySize);
    encoded.insert(encoded.end(), {'M', 'Y', 'D', 'B'});
    encoded.push_back(frame.header.version);
    encoded.push_back(frame.header.flags);
    appendU16(encoded, opcode);
    appendU16(encoded, frame.header.headerLength);
    appendU32(encoded, static_cast<std::uint32_t>(bodySize));
    appendU64(encoded, frame.header.requestId);
    appendU16(encoded, 0);

    for (const TlvField& field : frame.fields) {
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
