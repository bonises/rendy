#include <catch2/catch_test_macros.hpp>

#include "text/shaper.hpp"

#include <filesystem>
#include <numeric>

using namespace rendy;
using namespace rendy::text;

namespace {

const char* dejaVu() {
    for (const char* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                             "/usr/share/fonts/TTF/DejaVuSans.ttf"})
        if (std::filesystem::exists(path)) return path;
    return nullptr;
}

float totalAdvance(const std::vector<ShapedGlyph>& glyphs) {
    return std::accumulate(glyphs.begin(), glyphs.end(), 0.0f,
                           [](float sum, const ShapedGlyph& g) { return sum + g.xAdvance; });
}

} // namespace

TEST_CASE("shaper maps text to glyphs with kerning", "[text][shaper]") {
    const char* fontPath = dejaVu();
    if (fontPath == nullptr) {
        SUCCEED("no system DejaVu font — skipping");
        return;
    }
    Shaper shaper;
    auto font = shaper.loadFont(fontPath);
    REQUIRE(font.hasValue());
    const uint32_t id = font.value();

    std::vector<ShapedGlyph> glyphs;
    REQUIRE(shaper.shape(id, 16.0f, "abc", &glyphs));
    REQUIRE(glyphs.size() == 3);
    for (const ShapedGlyph& g : glyphs) {
        REQUIRE(g.glyphIndex != 0); // no .notdef for plain latin
        REQUIRE(g.xAdvance > 0.0f);
    }

    // Kerning: "AV" is tighter shaped together than the two advances apart.
    std::vector<ShapedGlyph> a, v, av;
    REQUIRE(shaper.shape(id, 32.0f, "A", &a));
    REQUIRE(shaper.shape(id, 32.0f, "V", &v));
    REQUIRE(shaper.shape(id, 32.0f, "AV", &av));
    REQUIRE(totalAdvance(av) < totalAdvance(a) + totalAdvance(v) - 0.1f);

    // Unknown font id fails; empty text succeeds with zero glyphs.
    REQUIRE_FALSE(shaper.shape(99, 16.0f, "x", &glyphs));
    REQUIRE(shaper.shape(id, 16.0f, "", &glyphs));
    REQUIRE(glyphs.empty());
}

TEST_CASE("shaper applies contextual arabic forms", "[text][shaper]") {
    const char* fontPath = dejaVu();
    if (fontPath == nullptr) {
        SUCCEED("no system DejaVu font — skipping");
        return;
    }
    Shaper shaper;
    const uint32_t id = shaper.loadFont(fontPath).value();

    // بب — the same letter (beh) twice: joining gives the two instances
    // DIFFERENT glyphs (initial + final forms). A codepoint-keyed renderer
    // would draw the same isolated form twice.
    std::vector<ShapedGlyph> glyphs;
    REQUIRE(shaper.shape(id, 16.0f, "بب", &glyphs));
    REQUIRE(glyphs.size() == 2);
    REQUIRE(glyphs[0].glyphIndex != 0);
    REQUIRE(glyphs[1].glyphIndex != 0);
    REQUIRE(glyphs[0].glyphIndex != glyphs[1].glyphIndex);
    // RTL output is in VISUAL order: the leftmost glyph is the logically
    // second letter (cluster = its byte offset, beh is 2 bytes).
    REQUIRE(glyphs[0].cluster == 2);
    REQUIRE(glyphs[1].cluster == 0);

    // Mixed line: the Latin prefix shapes LTR, the Arabic phrase RTL — the
    // two Arabic words come back with the logically-later word first
    // (leftmost). Cluster offsets: "hej " = 4 bytes, "سلام" = 8 bytes,
    // space at 12, "عليكم" starts at byte 13.
    REQUIRE(shaper.shape(id, 16.0f, "hej سلام عليكم", &glyphs));
    REQUIRE(glyphs.size() >= 8);
    REQUIRE(glyphs[0].cluster == 0);  // 'h'
    REQUIRE(glyphs[4].cluster >= 13); // first RTL-run glyph = leftmost = last word
}
