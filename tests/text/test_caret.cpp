#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "text/caret.hpp"
#include "text/shaper.hpp"

#include <filesystem>

using Catch::Approx;
using namespace rendy;
using namespace rendy::text;

namespace {

const char* dejaVu() {
    for (const char* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                             "/usr/share/fonts/TTF/DejaVuSans.ttf"})
        if (std::filesystem::exists(path)) return path;
    return nullptr;
}

float width(const std::vector<ShapedGlyph>& glyphs) {
    float total = 0.0f;
    for (const ShapedGlyph& g : glyphs) total += g.xAdvance;
    return total;
}

} // namespace

TEST_CASE("caret positions follow shaped LTR text", "[text][caret]") {
    const char* fontPath = dejaVu();
    if (fontPath == nullptr) {
        SUCCEED("no system DejaVu font — skipping");
        return;
    }
    Shaper shaper;
    const uint32_t id = shaper.loadFont(fontPath).value();
    const std::string_view line = "office";
    std::vector<ShapedGlyph> glyphs;
    REQUIRE(shaper.shape(id, 16.0f, line, &glyphs));

    // Endpoints: 0 at the left, size at the full shaped width — whatever
    // ligatures formed in between.
    REQUIRE(caretX(glyphs, line, 0) == Approx(0.0f));
    REQUIRE(caretX(glyphs, line, line.size()) == Approx(width(glyphs)));

    // Strictly monotone across byte offsets (interpolated inside any
    // ligature cluster).
    float previous = -1.0f;
    for (size_t b = 0; b <= line.size(); ++b) {
        const float x = caretX(glyphs, line, b);
        REQUIRE(x > previous);
        previous = x;
    }

    // Hit-testing roundtrips every codepoint boundary.
    for (size_t b = 0; b <= line.size(); ++b)
        REQUIRE(caretFromX(glyphs, line, caretX(glyphs, line, b)) == b);

    // Empty line: everything lands at 0.
    std::vector<ShapedGlyph> none;
    REQUIRE(caretX(none, "", 0) == 0.0f);
    REQUIRE(caretFromX(none, "", 123.0f) == 0);
}

TEST_CASE("caret positions mirror in RTL text", "[text][caret]") {
    const char* fontPath = dejaVu();
    if (fontPath == nullptr) {
        SUCCEED("no system DejaVu font — skipping");
        return;
    }
    Shaper shaper;
    const uint32_t id = shaper.loadFont(fontPath).value();
    const std::string_view line = "بب"; // two beh, 2 bytes each
    std::vector<ShapedGlyph> glyphs;
    REQUIRE(shaper.shape(id, 16.0f, line, &glyphs));

    // Logical start renders at the RIGHT edge, logical end at the LEFT.
    const float start = caretX(glyphs, line, 0);
    const float middle = caretX(glyphs, line, 2);
    const float end = caretX(glyphs, line, 4);
    REQUIRE(start == Approx(width(glyphs)));
    REQUIRE(end == Approx(0.0f));
    REQUIRE(middle < start);
    REQUIRE(middle > end);

    // Hit-test agrees: clicking the right edge = offset 0.
    REQUIRE(caretFromX(glyphs, line, start) == 0);
    REQUIRE(caretFromX(glyphs, line, end) == 4);
}

TEST_CASE("cluster breaks keep combining marks attached", "[text][caret]") {
    const char* fontPath = dejaVu();
    if (fontPath == nullptr) {
        SUCCEED("no system DejaVu font — skipping");
        return;
    }
    Shaper shaper;
    const uint32_t id = shaper.loadFont(fontPath).value();
    // "a" + U+0301 combining acute (2 bytes) + "b": the mark merges into
    // a's cluster, so byte 1 is NOT a valid break.
    const std::string_view line = "a\xcc\x81"
                                  "b";
    std::vector<ShapedGlyph> glyphs;
    REQUIRE(shaper.shape(id, 16.0f, line, &glyphs));

    std::vector<size_t> breaks;
    clusterBreaks(glyphs, line.size(), &breaks);
    REQUIRE(breaks.front() == 0);
    REQUIRE(breaks.back() == line.size());
    for (size_t offset : breaks) REQUIRE(offset != 1); // inside a+mark cluster
}
