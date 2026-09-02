#include <catch2/catch_test_macros.hpp>

#include "text/utf8.hpp"

#include <cstdint>
#include <vector>

using rendy::text::decodeUtf8;
using rendy::text::kReplacementChar;

namespace {
std::vector<uint32_t> decodeAll(std::string_view str) {
    std::vector<uint32_t> out;
    size_t offset = 0;
    while (offset < str.size()) out.push_back(decodeUtf8(str, offset));
    return out;
}
} // namespace

TEST_CASE("ASCII decodes 1:1", "[text][utf8]") {
    REQUIRE(decodeAll("Hi!") == std::vector<uint32_t>{'H', 'i', '!'});
}

TEST_CASE("Swedish and multibyte text", "[text][utf8]") {
    // å = U+00E5 (2 bytes), € = U+20AC (3 bytes), 🎨 = U+1F3A8 (4 bytes)
    REQUIRE(decodeAll("åäö") == std::vector<uint32_t>{0xE5, 0xE4, 0xF6});
    REQUIRE(decodeAll("€") == std::vector<uint32_t>{0x20AC});
    REQUIRE(decodeAll("🎨") == std::vector<uint32_t>{0x1F3A8});
}

TEST_CASE("invalid sequences become U+FFFD and always advance", "[text][utf8]") {
    // Lone continuation byte.
    REQUIRE(decodeAll("\x80") == std::vector<uint32_t>{kReplacementChar});
    // Truncated 3-byte sequence at end of string.
    REQUIRE(decodeAll("\xE2\x82") ==
            std::vector<uint32_t>{kReplacementChar, kReplacementChar});
    // Overlong encoding of '/' (0xC0 0xAF).
    const auto overlong = decodeAll("\xC0\xAF");
    REQUIRE(overlong.front() == kReplacementChar);
    // Surrogate half U+D800 encoded as UTF-8.
    REQUIRE(decodeAll("\xED\xA0\x80") == std::vector<uint32_t>{kReplacementChar});
}

TEST_CASE("decoding always terminates on garbage", "[text][utf8]") {
    std::string garbage;
    for (int i = 0; i < 256; ++i) garbage.push_back(static_cast<char>(i));
    size_t offset = 0;
    size_t steps = 0;
    while (offset < garbage.size() && steps < 10000) {
        decodeUtf8(garbage, offset);
        steps++;
    }
    REQUIRE(offset == garbage.size());
}
