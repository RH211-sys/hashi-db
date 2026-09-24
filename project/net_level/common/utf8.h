#pragma once
#ifndef _MYDB_NET_UTF8_H_
#define _MYDB_NET_UTF8_H_

#include <cstddef>
#include <cstdint>

namespace mydb::net {

inline bool isValidUtf8(const std::uint8_t* bytes, std::size_t length) noexcept {
    std::size_t index = 0;
    while (index < length) {
        const std::uint8_t lead = bytes[index];
        if (lead <= 0x7fU) {
            ++index;
            continue;
        }

        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        if (lead >= 0xc2U && lead <= 0xdfU) {
            continuationCount = 1;
            codePoint = lead & 0x1fU;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            continuationCount = 2;
            codePoint = lead & 0x0fU;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            continuationCount = 3;
            codePoint = lead & 0x07U;
        } else {
            return false;
        }
        if (continuationCount > length - index - 1) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
            const std::uint8_t continuation = bytes[index + offset];
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            codePoint = (codePoint << 6U) | (continuation & 0x3fU);
        }
        if ((continuationCount == 2 && codePoint < 0x800U) ||
            (continuationCount == 3 && codePoint < 0x10000U) ||
            (codePoint >= 0xd800U && codePoint <= 0xdfffU) || codePoint > 0x10ffffU) {
            return false;
        }
        index += continuationCount + 1;
    }
    return true;
}

}

#endif
