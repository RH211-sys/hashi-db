#pragma once
#ifndef _MYDB_NET_UTF8_H_
#define _MYDB_NET_UTF8_H_

/*
    模块名：UTF-8 校验工具
    模块地位：命令解析使用的公共字符编码校验模块。
    模块功能描述：验证字节序列是否符合 UTF-8 编码规则。
*/

#include <cstddef>
#include <cstdint>

namespace mydb::net {

/*
    函数：isValidUtf8
    功能：检查字节序列是否符合 UTF-8 编码规则。
    传参：bytes：待检查数据；length：数据字节数。
    返回值：数据合法时返回 true，否则返回 false。
*/
inline bool isValidUtf8(const std::uint8_t* bytes, std::size_t length) noexcept {
    std::size_t index = 0;                       // 字节游标：指向当前待校验的 UTF-8 首字节
    while (index < length) {
        const std::uint8_t lead = bytes[index];  // 首字节：决定码点长度和有效位数
        if (lead <= 0x7fU) {
            // ASCII 字节本身构成一个合法码点，前进到下一个字节。
            ++index;
            continue;
        }

        std::size_t continuationCount = 0;       // 续字节数：当前码点还需读取的字节数量
        std::uint32_t codePoint = 0;             // 码点：逐步累积当前 UTF-8 字符的 Unicode 值
        if (lead >= 0xc2U && lead <= 0xdfU) {
            // 首字节符合二字节编码范围，初始化一个续字节的码点。
            continuationCount = 1;
            codePoint = lead & 0x1fU;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            // 首字节符合三字节编码范围，初始化两个续字节的码点。
            continuationCount = 2;
            codePoint = lead & 0x0fU;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            // 首字节符合四字节编码范围，初始化三个续字节的码点。
            continuationCount = 3;
            codePoint = lead & 0x07U;
        } else {
            // 首字节不属于允许的 UTF-8 起始范围，拒绝该字节序列。
            return false;
        }
        if (continuationCount > length - index - 1) {
            // 当前码点缺少所需续字节，输入序列不完整。
            return false;
        }
        for (std::size_t offset = 1; offset <= continuationCount; ++offset) { // 续字节偏移：遍历当前码点的后续字节
            const std::uint8_t continuation = bytes[index + offset]; // 续字节：参与码点累积并校验格式
            if ((continuation & 0xc0U) != 0x80U) {
                // 当前字节不符合 UTF-8 续字节格式，拒绝该序列。
                return false;
            }
            codePoint = (codePoint << 6U) | (continuation & 0x3fU);
        }
        if ((continuationCount == 2 && codePoint < 0x800U) ||
            (continuationCount == 3 && codePoint < 0x10000U) ||
            (codePoint >= 0xd800U && codePoint <= 0xdfffU) || codePoint > 0x10ffffU) {
            // 码点存在过长编码、代理区编码或超出 Unicode 范围，拒绝该序列。
            return false;
        }
        index += continuationCount + 1;
    }
    return true;
}

}

#endif
