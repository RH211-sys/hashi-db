/*
    模块名：命令解析模块
    功能描述：将协议层完整 Frame 校验并转换为命令请求，负责字段提取和基础参数校验，不调用存储层。
*/

#include "command_parser.h"
#include "protocol_fields.h"
#include "../common/utf8.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace mydb::net {
namespace {

constexpr std::uint16_t FIELD_VERSION = fieldId(FieldId::PROTOCOL_VERSION);
constexpr std::uint16_t FIELD_FEATURES = fieldId(FieldId::FEATURE_BITS);
constexpr std::uint16_t FIELD_CLIENT_NAME = fieldId(FieldId::CLIENT_NAME);
constexpr std::uint16_t FIELD_USERNAME = fieldId(FieldId::USERNAME);
constexpr std::uint16_t FIELD_PASSWORD = fieldId(FieldId::PASSWORD);
constexpr std::uint16_t FIELD_PAYLOAD = fieldId(FieldId::PAYLOAD);
constexpr std::uint16_t FIELD_KEY = fieldId(FieldId::KEY);
constexpr std::uint16_t FIELD_TYPE = fieldId(FieldId::TYPE_NAME);
constexpr std::uint16_t FIELD_VALUE = fieldId(FieldId::VALUE);
constexpr std::uint16_t FIELD_TTL = fieldId(FieldId::TTL);
constexpr std::uint16_t FIELD_SCOPE = fieldId(FieldId::SCOPE);
constexpr std::uint16_t FIELD_CONNECTION_ID = fieldId(FieldId::CONNECTION_ID);
constexpr std::uint16_t FIELD_STATUS_CODE = fieldId(FieldId::STATUS_CODE);
constexpr std::uint16_t FIELD_MESSAGE = fieldId(FieldId::MESSAGE);

struct FieldRule {
    std::uint16_t id;
    FieldType type;
    bool required;
    std::size_t maxLength;
};

constexpr std::size_t NO_LENGTH_LIMIT = std::numeric_limits<std::size_t>::max();
constexpr std::array<FieldRule, 14> ALL_FIELDS{{
    {FIELD_VERSION, FieldType::BYTES, true, 2},
    {FIELD_FEATURES, FieldType::U64, false, 8},
    {FIELD_CLIENT_NAME, FieldType::UTF8, false, NO_LENGTH_LIMIT},
    {FIELD_USERNAME, FieldType::UTF8, true, NO_LENGTH_LIMIT},
    {FIELD_PASSWORD, FieldType::BYTES, true, NO_LENGTH_LIMIT},
    {FIELD_PAYLOAD, FieldType::BYTES, false, NO_LENGTH_LIMIT},
    {FIELD_KEY, FieldType::BYTES, true, 32},
    {FIELD_TYPE, FieldType::UTF8, true, 32},
    {FIELD_VALUE, FieldType::BYTES, true, NO_LENGTH_LIMIT},
    {FIELD_TTL, FieldType::U64, false, 8},
    {FIELD_SCOPE, FieldType::UTF8, false, NO_LENGTH_LIMIT},
    {FIELD_CONNECTION_ID, FieldType::U64, true, 8},
    {FIELD_STATUS_CODE, FieldType::U64, true, 8},
    {FIELD_MESSAGE, FieldType::UTF8, false, NO_LENGTH_LIMIT}
}};

struct OpcodeRules {
    Opcode opcode;
    const FieldRule* fields;
    std::size_t fieldCount;
};

constexpr std::array<FieldRule, 3> HELLO_FIELDS{{
    ALL_FIELDS[0], ALL_FIELDS[2], ALL_FIELDS[1]
}};
constexpr std::array<FieldRule, 2> AUTH_FIELDS{{
    ALL_FIELDS[3], ALL_FIELDS[4]
}};
constexpr std::array<FieldRule, 1> PING_FIELDS{{ALL_FIELDS[5]}};
constexpr std::array<FieldRule, 0> NO_FIELDS{};
constexpr std::array<FieldRule, 1> KEY_FIELDS{{ALL_FIELDS[6]}};
constexpr std::array<FieldRule, 4> ADD_FIELDS{{
    ALL_FIELDS[6], ALL_FIELDS[7], ALL_FIELDS[8],
    {FIELD_TTL, FieldType::U64, false, 8}
}};
constexpr std::array<FieldRule, 3> UPDATE_FIELDS{{
    ALL_FIELDS[6], ALL_FIELDS[7], ALL_FIELDS[8]
}};
constexpr std::array<FieldRule, 1> PERSIST_FIELDS{{
    {FIELD_KEY, FieldType::BYTES, false, 32}
}};
constexpr std::array<FieldRule, 1> STATS_FIELDS{{
    ALL_FIELDS[10]
}};
constexpr std::array<FieldRule, 1> CLIENT_KILL_FIELDS{{
    ALL_FIELDS[11]
}};

constexpr std::array<OpcodeRules, 14> OPCODE_RULES{{
    {Opcode::HELLO, HELLO_FIELDS.data(), HELLO_FIELDS.size()},
    {Opcode::AUTH, AUTH_FIELDS.data(), AUTH_FIELDS.size()},
    {Opcode::PING, PING_FIELDS.data(), PING_FIELDS.size()},
    {Opcode::QUIT, NO_FIELDS.data(), NO_FIELDS.size()},
    {Opcode::GET, KEY_FIELDS.data(), KEY_FIELDS.size()},
    {Opcode::ADD, ADD_FIELDS.data(), ADD_FIELDS.size()},
    {Opcode::UPDATE, UPDATE_FIELDS.data(), UPDATE_FIELDS.size()},
    {Opcode::DELETE_DATA, KEY_FIELDS.data(), KEY_FIELDS.size()},
    {Opcode::PERSIST, PERSIST_FIELDS.data(), PERSIST_FIELDS.size()},
    {Opcode::FLUSH, NO_FIELDS.data(), NO_FIELDS.size()},
    {Opcode::REWRITE, NO_FIELDS.data(), NO_FIELDS.size()},
    {Opcode::STATS, STATS_FIELDS.data(), STATS_FIELDS.size()},
    {Opcode::CLIENT_LIST, NO_FIELDS.data(), NO_FIELDS.size()},
    {Opcode::CLIENT_KILL, CLIENT_KILL_FIELDS.data(), CLIENT_KILL_FIELDS.size()}
}};

void fail(ErrorCode code, std::string_view text, ErrorCode& error, std::string& message) {
    error = code;
    message.assign(text);
}

const OpcodeRules* findRules(Opcode opcode) {
    for (const OpcodeRules& rules : OPCODE_RULES) {
        if (rules.opcode == opcode) {
            return &rules;
        }
    }
    return nullptr;
}

const FieldRule* findFieldRule(const OpcodeRules& rules, std::uint16_t fieldId) {
    for (std::size_t index = 0; index < rules.fieldCount; ++index) {
        if (rules.fields[index].id == fieldId) {
            return &rules.fields[index];
        }
    }
    return nullptr;
}

bool isKnownFieldId(std::uint16_t fieldId) {
    return fieldId >= FIELD_VERSION && fieldId <= FIELD_MESSAGE;
}

bool validTypeValue(const TlvField& field) {
    switch (field.type) {
    case FieldType::BYTES:
        return true;
    case FieldType::UTF8:
        return isValidUtf8(field.value.data(), field.value.size());
    case FieldType::U64:
    case FieldType::I64:
        return field.value.size() == 8;
    case FieldType::BOOL:
        return field.value.size() == 1 && field.value.front() <= 1;
    case FieldType::NESTED:
        // Command schemas in protocol version 1 do not currently define nested fields.
        return false;
    }
    return false;
}

void setError(ErrorCode code, std::string_view text, ErrorCode& error, std::string& message) {
    fail(code, text, error, message);
}

} // namespace

bool CommandParser::parse(Frame&& frame, ConnectionId connectionId, CommandRequest& request,
                          ErrorCode& error, std::string& message, std::uint32_t maxFrameBytes) const {
    request = CommandRequest{};
    error = ErrorCode::OK;
    message.clear();

    if (frame.header.version != PROTOCOL_VERSION || frame.header.headerLength != BASE_HEADER_LENGTH ||
        maxFrameBytes < BASE_HEADER_LENGTH || frame.header.bodyLength > maxFrameBytes - BASE_HEADER_LENGTH) {
        setError(ErrorCode::BAD_FRAME, "invalid frame header", error, message);
        return false;
    }

    if ((frame.header.flags & RESPONSE) != 0 || (frame.header.flags & ERROR) != 0) {
        setError(ErrorCode::BAD_FRAME, "request frame has response flags", error, message);
        return false;
    }
    constexpr std::uint8_t supportedRequestFlags = 0;
    if ((frame.header.flags & static_cast<std::uint8_t>(~supportedRequestFlags)) != 0) {
        setError(ErrorCode::UNSUPPORTED, "unsupported request flags", error, message);
        return false;
    }

    const OpcodeRules* rules = findRules(frame.header.opcode);
    if (rules == nullptr) {
        setError(ErrorCode::UNKNOWN_COMMAND, "unknown opcode", error, message);
        return false;
    }

    std::size_t encodedBodyLength = 0;
    for (const TlvField& field : frame.fields) {
        if (field.value.size() > std::numeric_limits<std::uint32_t>::max() ||
            field.value.size() > std::numeric_limits<std::size_t>::max() - 8U ||
            encodedBodyLength > std::numeric_limits<std::size_t>::max() - 8U - field.value.size()) {
            setError(ErrorCode::TOO_LARGE, "field length exceeds protocol limit", error, message);
            return false;
        }
        encodedBodyLength += 8U + field.value.size();
    }
    if (encodedBodyLength != frame.header.bodyLength) {
        setError(ErrorCode::BAD_FRAME, "body length does not match fields", error, message);
        return false;
    }

    std::array<bool, 15> seen{};
    for (const TlvField& field : frame.fields) {
        if (field.fieldId >= seen.size() || !isKnownFieldId(field.fieldId)) {
            setError(ErrorCode::UNSUPPORTED, "unknown field id", error, message);
            return false;
        }
        const FieldRule* fieldRule = findFieldRule(*rules, field.fieldId);
        if (fieldRule == nullptr) {
            setError(ErrorCode::BAD_FRAME, "field is not allowed for opcode", error, message);
            return false;
        }
        if (seen[field.fieldId]) {
            setError(ErrorCode::BAD_FRAME, "duplicate field", error, message);
            return false;
        }
        seen[field.fieldId] = true;

        if (field.type != fieldRule->type || !validTypeValue(field)) {
            setError(ErrorCode::BAD_FRAME, "field type or value length is invalid", error, message);
            return false;
        }
        if (field.value.size() > fieldRule->maxLength) {
            setError(ErrorCode::TOO_LARGE, "field exceeds length limit", error, message);
            return false;
        }
    }

    for (std::size_t index = 0; index < rules->fieldCount; ++index) {
        const FieldRule& fieldRule = rules->fields[index];
        if (fieldRule.required && !seen[fieldRule.id]) {
            setError(ErrorCode::BAD_FRAME, "required field is missing", error, message);
            return false;
        }
    }

    if (frame.header.opcode == Opcode::HELLO) {
        const auto versionField = std::find_if(frame.fields.begin(), frame.fields.end(), [](const TlvField& field) {
            return field.fieldId == FIELD_VERSION;
        });
        if (versionField == frame.fields.end() || versionField->value.size() != 2 ||
            versionField->value[0] != 0 || versionField->value[1] != PROTOCOL_VERSION) {
            setError(ErrorCode::UNSUPPORTED, "unsupported protocol version", error, message);
            return false;
        }
    }

    request.connectionId = connectionId;
    request.requestId = frame.header.requestId;
    request.source = RequestSource::REMOTE;
    request.opcode = frame.header.opcode;
    request.fields = std::move(frame.fields);
    return true;
}

} // namespace mydb::net
