/*
    模块名：命令解析模块
    模块地位：实现协议字段规则校验和结构化请求构造。
    模块功能描述：将协议层完整 Frame 校验并转换为命令请求，负责字段提取和基础参数校验，不调用存储层。
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

constexpr std::uint16_t FIELD_VERSION = fieldId(FieldId::PROTOCOL_VERSION);             // 字段编号：协议版本
constexpr std::uint16_t FIELD_FEATURES = fieldId(FieldId::FEATURE_BITS);                // 字段编号：协商特性位
constexpr std::uint16_t FIELD_CLIENT_NAME = fieldId(FieldId::CLIENT_NAME);              // 字段编号：客户端名称
constexpr std::uint16_t FIELD_USERNAME = fieldId(FieldId::USERNAME);                    // 字段编号：认证用户名
constexpr std::uint16_t FIELD_PASSWORD = fieldId(FieldId::PASSWORD);                    // 字段编号：认证口令
constexpr std::uint16_t FIELD_PAYLOAD = fieldId(FieldId::PAYLOAD);                      // 字段编号：命令负载
constexpr std::uint16_t FIELD_KEY = fieldId(FieldId::KEY);                              // 字段编号：存储键
constexpr std::uint16_t FIELD_TYPE = fieldId(FieldId::TYPE_NAME);                       // 字段编号：存储类型名
constexpr std::uint16_t FIELD_VALUE = fieldId(FieldId::VALUE);                          // 字段编号：存储值
constexpr std::uint16_t FIELD_TTL = fieldId(FieldId::TTL);                              // 字段编号：生存时间
constexpr std::uint16_t FIELD_SCOPE = fieldId(FieldId::SCOPE);                          // 字段编号：操作范围
constexpr std::uint16_t FIELD_CONNECTION_ID = fieldId(FieldId::CONNECTION_ID);          // 字段编号：连接标识
constexpr std::uint16_t FIELD_STATUS_CODE = fieldId(FieldId::STATUS_CODE);              // 字段编号：响应状态码
constexpr std::uint16_t FIELD_MESSAGE = fieldId(FieldId::MESSAGE);                      // 字段编号：响应说明文本

/*
    地位：协议字段定义表中的单字段约束。
    功能：描述字段编号、类型、必需性和最大长度。
*/
struct FieldRule {
    std::uint16_t id;                             // 字段编号：对应协议 FieldId
    FieldType type;                               // 字段类型：限制 TLV 值的编码类型
    bool required;                                // 必需标记：命令是否必须包含该字段
    std::size_t maxLength;                        // 长度上限：限制该字段值的最大字节数
};

constexpr std::size_t NO_LENGTH_LIMIT = std::numeric_limits<std::size_t>::max(); // 无限制标记：字段不设额外字节长度上限
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
}}; // 全字段表：集中定义协议字段的类型、必需性和长度限制

/*
    地位：命令码与其字段规则集合之间的关联项。
    功能：为解析器提供一条命令允许字段的只读规则视图。
*/
struct OpcodeRules {
    Opcode opcode;                                // 命令码：标识本规则对应的操作
    const FieldRule* fields;                      // 规则数组：指向允许字段定义
    std::size_t fieldCount;                       // 规则数量：允许字段定义的元素数
};

constexpr std::array<FieldRule, 3> HELLO_FIELDS{{
    ALL_FIELDS[0], ALL_FIELDS[2], ALL_FIELDS[1]
}}; // HELLO 规则：允许版本、客户端名称和特性位字段

constexpr std::array<FieldRule, 2> AUTH_FIELDS{{
    ALL_FIELDS[3], ALL_FIELDS[4]
}}; // AUTH 规则：要求用户名和口令字段

constexpr std::array<FieldRule, 1> PING_FIELDS{{ALL_FIELDS[5]}}; // PING 规则：允许负载字段
constexpr std::array<FieldRule, 0> NO_FIELDS{}; // 空规则：用于不接受字段的命令
constexpr std::array<FieldRule, 1> KEY_FIELDS{{ALL_FIELDS[6]}}; // 键规则：要求存储键字段
constexpr std::array<FieldRule, 4> ADD_FIELDS{{
    ALL_FIELDS[6], ALL_FIELDS[7], ALL_FIELDS[8],
    {FIELD_TTL, FieldType::U64, false, 8}
}}; // ADD 规则：要求键、类型和值，可选 TTL
constexpr std::array<FieldRule, 3> UPDATE_FIELDS{{
    ALL_FIELDS[6], ALL_FIELDS[7], ALL_FIELDS[8]
}}; // UPDATE 规则：要求键、类型和值
constexpr std::array<FieldRule, 1> PERSIST_FIELDS{{
    {FIELD_KEY, FieldType::BYTES, false, 32}
}}; // PERSIST 规则：可选指定持久化键
constexpr std::array<FieldRule, 1> STATS_FIELDS{{
    ALL_FIELDS[10]
}}; // STATS 规则：允许范围字段
constexpr std::array<FieldRule, 1> CLIENT_KILL_FIELDS{{
    ALL_FIELDS[11]
}}; // CLIENT_KILL 规则：要求连接标识字段

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
}}; // 命令规则表：将每种操作码映射到其允许字段集合

/*
    函数：fail
    传参：code：解析错误码；text：错误说明；error：错误码输出；message：错误文本输出
    功能：设置解析失败时返回的错误信息
    返回值：无
*/
void fail(ErrorCode code, std::string_view text, ErrorCode& error, std::string& message) {
    error = code;
    message.assign(text);
}

/*
    函数：findRules
    传参：opcode：待查找的命令码
    功能：在命令规则表中查找对应字段约束
    返回值：找到时返回规则地址；未找到时返回空指针
*/
const OpcodeRules* findRules(Opcode opcode) {
    for (const OpcodeRules& rules : OPCODE_RULES) { // 命令规则：查找与操作码匹配的字段集合
        if (rules.opcode == opcode) {
            // 当前规则对应目标命令码，返回该命令的字段约束。
            return &rules;
        }
    }
    return nullptr;
}

/*
    函数：findFieldRule
    传参：rules：当前命令的字段规则；fieldId：待查找字段编号
    功能：在命令允许字段列表中查找指定字段约束
    返回值：找到时返回字段规则地址；未找到时返回空指针
*/
const FieldRule* findFieldRule(const OpcodeRules& rules, std::uint16_t fieldId) {
    for (std::size_t index = 0; index < rules.fieldCount; ++index) { // 规则下标：遍历该命令的字段约束
        if (rules.fields[index].id == fieldId) {
            // 当前规则字段编号匹配目标编号，返回对应约束项。
            return &rules.fields[index];
        }
    }
    return nullptr;
}

/*
    函数：isKnownFieldId
    传参：fieldId：请求中的协议字段编号
    功能：判断字段编号是否位于当前协议定义范围
    返回值：字段编号已定义时为 true
*/
bool isKnownFieldId(std::uint16_t fieldId) {
    return fieldId >= FIELD_VERSION && fieldId <= FIELD_MESSAGE;
}

/*
    函数：validTypeValue
    传参：field：待检查的 TLV 字段
    功能：按字段类型检查字段值的长度和文本编码
    返回值：字段值符合类型约束时为 true
*/
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
        // 协议 v1 的命令字段规则当前未定义嵌套字段。
        return false;
    }
    return false;
}

/*
    函数：setError
    传参：code：解析错误码；text：错误说明；error：错误码输出；message：错误文本输出
    功能：统一设置解析失败结果
    返回值：无
*/
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
        // 帧头版本、长度或配置上限不符合协议，返回帧格式错误。
        setError(ErrorCode::BAD_FRAME, "invalid frame header", error, message);
        return false;
    }

    if ((frame.header.flags & RESPONSE) != 0 || (frame.header.flags & ERROR) != 0) {
        // 请求帧携带响应或错误标志，拒绝作为客户端请求解析。
        setError(ErrorCode::BAD_FRAME, "request frame has response flags", error, message);
        return false;
    }
    constexpr std::uint8_t supportedRequestFlags = 0; // 支持标志：当前请求协议未定义可用标志位
    if ((frame.header.flags & static_cast<std::uint8_t>(~supportedRequestFlags)) != 0) {
        // 请求帧设置了当前版本不支持的标志位，返回不支持错误。
        setError(ErrorCode::UNSUPPORTED, "unsupported request flags", error, message);
        return false;
    }

    const OpcodeRules* rules = findRules(frame.header.opcode); // 命令规则：取得操作码对应的字段约束
    if (rules == nullptr) {
        // 命令码不在规则表中，返回未知命令错误。
        setError(ErrorCode::UNKNOWN_COMMAND, "unknown opcode", error, message);
        return false;
    }

    std::size_t encodedBodyLength = 0;             // Body 字节数：累计字段头和值的编码长度
    for (const TlvField& field : frame.fields) {   // 当前字段：校验编码长度并累计 Body 大小
        if (field.value.size() > std::numeric_limits<std::uint32_t>::max() ||
            field.value.size() > std::numeric_limits<std::size_t>::max() - 8U ||
            encodedBodyLength > std::numeric_limits<std::size_t>::max() - 8U - field.value.size()) {
            // 单字段长度或累计 Body 长度超出可表达范围，返回过大错误。
            setError(ErrorCode::TOO_LARGE, "field length exceeds protocol limit", error, message);
            return false;
        }
        encodedBodyLength += 8U + field.value.size();
    }
    if (encodedBodyLength != frame.header.bodyLength) {
        // 字段实际编码长度与帧头声明不一致，返回帧格式错误。
        setError(ErrorCode::BAD_FRAME, "body length does not match fields", error, message);
        return false;
    }

    std::array<bool, 15> seen{};                    // 出现记录：按字段编号标记请求中已见字段
    for (const TlvField& field : frame.fields) {   // 当前字段：检查命令允许性、重复性及值类型
        if (field.fieldId >= seen.size() || !isKnownFieldId(field.fieldId)) {
            // 字段编号未定义或超出跟踪范围，返回不支持错误。
            setError(ErrorCode::UNSUPPORTED, "unknown field id", error, message);
            return false;
        }
        const FieldRule* fieldRule = findFieldRule(*rules, field.fieldId); // 字段规则：取得当前字段的命令约束
        if (fieldRule == nullptr) {
            // 当前命令不允许该字段，返回帧格式错误。
            setError(ErrorCode::BAD_FRAME, "field is not allowed for opcode", error, message);
            return false;
        }
        if (seen[field.fieldId]) {
            // 同一字段编号已出现，拒绝重复字段。
            setError(ErrorCode::BAD_FRAME, "duplicate field", error, message);
            return false;
        }
        seen[field.fieldId] = true;

        if (field.type != fieldRule->type || !validTypeValue(field)) {
            // 字段类型或值长度不符合规则定义，返回帧格式错误。
            setError(ErrorCode::BAD_FRAME, "field type or value length is invalid", error, message);
            return false;
        }
        if (field.value.size() > fieldRule->maxLength) {
            // 字段值超过该命令规则设置的长度上限，返回过大错误。
            setError(ErrorCode::TOO_LARGE, "field exceeds length limit", error, message);
            return false;
        }
    }

    for (std::size_t index = 0; index < rules->fieldCount; ++index) { // 规则下标：检查每项必需字段是否出现
        const FieldRule& fieldRule = rules->fields[index]; // 当前规则：决定字段是否必须出现在请求中
        if (fieldRule.required && !seen[fieldRule.id]) {
            // 必需字段未出现在请求中，返回帧格式错误。
            setError(ErrorCode::BAD_FRAME, "required field is missing", error, message);
            return false;
        }
    }

    if (frame.header.opcode == Opcode::HELLO) {
        // HELLO 需要额外验证版本字段存在且与当前协议版本匹配。
        const auto versionField = std::find_if(frame.fields.begin(), frame.fields.end(), [](const TlvField& field) { // field：当前待匹配的 HELLO 字段
            return field.fieldId == FIELD_VERSION;
        }); // 版本字段：指向 HELLO 中的协议版本协商字段
        if (versionField == frame.fields.end() || versionField->value.size() != 2 ||
            versionField->value[0] != 0 || versionField->value[1] != PROTOCOL_VERSION) {
            // HELLO 版本字段缺失、长度错误或版本不支持，返回不支持错误。
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
