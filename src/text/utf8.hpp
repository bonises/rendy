#pragma once

// Minimal UTF-8 decoding. Invalid sequences decode to U+FFFD and advance one
// byte, so garbage input still renders something instead of looping.

#include <cstdint>
#include <string_view>

namespace rendy::text {

inline constexpr uint32_t kReplacementChar = 0xFFFD;

/// Decodes the codepoint starting at `offset`; advances `offset`.
inline uint32_t decodeUtf8(std::string_view str, size_t& offset) {
    const auto byte = [&](size_t i) -> uint32_t {
        return static_cast<uint8_t>(str[offset + i]);
    };
    const uint32_t first = byte(0);

    if (first < 0x80) {
        offset += 1;
        return first;
    }

    int length = 0;
    uint32_t codepoint = 0;
    if ((first & 0xE0) == 0xC0) {
        length = 2;
        codepoint = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        length = 3;
        codepoint = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        length = 4;
        codepoint = first & 0x07;
    } else {
        offset += 1;
        return kReplacementChar;
    }

    if (offset + static_cast<size_t>(length) > str.size()) {
        offset += 1;
        return kReplacementChar;
    }
    for (int i = 1; i < length; ++i) {
        const uint32_t continuation = byte(static_cast<size_t>(i));
        if ((continuation & 0xC0) != 0x80) {
            offset += 1;
            return kReplacementChar;
        }
        codepoint = (codepoint << 6) | (continuation & 0x3F);
    }
    offset += static_cast<size_t>(length);

    // Reject overlong encodings and surrogates.
    if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
        (length == 4 && codepoint < 0x10000) || (codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
        codepoint > 0x10FFFF)
        return kReplacementChar;
    return codepoint;
}

} // namespace rendy::text
