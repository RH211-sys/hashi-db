#pragma once
#ifndef _MYDB_NET_PROTOCOL_FIELDS_H_
#define _MYDB_NET_PROTOCOL_FIELDS_H_

/*
    模块名：协议字段编号
    功能描述：定义协议 v1 TLV field_id。数值属于 wire contract，必须与协议设计文档同步维护。
*/

#include <cstdint>

namespace mydb::net {

enum class FieldId : std::uint16_t {
    PROTOCOL_VERSION = 1,
    FEATURE_BITS = 2,
    CLIENT_NAME = 3,
    USERNAME = 4,
    PASSWORD = 5,
    PAYLOAD = 6,
    KEY = 7,
    TYPE_NAME = 8,
    VALUE = 9,
    TTL = 10,
    SCOPE = 11,
    CONNECTION_ID = 12,
    STATUS_CODE = 13,
    MESSAGE = 14
};

constexpr std::uint16_t fieldId(FieldId id) noexcept {
    return static_cast<std::uint16_t>(id);
}

}

#endif
