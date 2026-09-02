#include <catch2/catch_test_macros.hpp>

#include "text/shaper.hpp"

#include <cstdint>
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

TEST_CASE("full bidi reorders runs in an RTL-base paragraph", "[text][shaper]") {
    const char* fontPath = dejaVu();
    if (fontPath == nullptr) {
        SUCCEED("no system DejaVu font — skipping");
        return;
    }
    Shaper shaper;
    const uint32_t id = shaper.loadFont(fontPath).value();

    // "سلام abc عليكم": the first strong character is Arabic, so the base
    // direction is RTL. Visually the line reads right-to-left: سلام
    // rightmost, عليكم leftmost, "abc" LTR in the middle. Byte offsets:
    // سلام = 0..7, space 8, abc = 9..11, space 12, عليكم = 13..22.
    std::vector<ShapedGlyph> glyphs;
    REQUIRE(shaper.shape(id, 16.0f, "سلام abc عليكم", &glyphs));
    REQUIRE(glyphs.size() >= 9);
    // Leftmost glyph comes from the logically-last word…
    REQUIRE(glyphs.front().cluster >= 13);
    // …and the rightmost from the first word.
    REQUIRE(glyphs.back().cluster <= 6);
    // The embedded "abc" keeps left-to-right order (clusters 9,10,11).
    bool foundAbc = false;
    for (size_t i = 0; i + 2 < glyphs.size(); ++i)
        if (glyphs[i].cluster == 9 && glyphs[i + 1].cluster == 10 &&
            glyphs[i + 2].cluster == 11)
            foundAbc = true;
    REQUIRE(foundAbc);

    // Digits in RTL text are numbers, not mirrored glyph soup: in
    // "سلام 123" (base RTL) the digit run sits leftmost and keeps 1-2-3
    // order. The direction-neutral splitter this replaced pulled digits
    // into the Arabic run and reversed them.
    REQUIRE(shaper.shape(id, 16.0f, "سلام 123", &glyphs));
    REQUIRE(glyphs.size() == 7); // 1 2 3 space م لا س (lam-alef ligates)
    REQUIRE(glyphs[0].cluster == 9);
    REQUIRE(glyphs[1].cluster == 10);
    REQUIRE(glyphs[2].cluster == 11);
    REQUIRE(glyphs.back().cluster == 0);
}

TEST_CASE("bidi rule L1: trailing whitespace resets to the base level",
          "[text][shaper]") {
    const char* fontPath = dejaVu();
    if (fontPath == nullptr) {
        SUCCEED("no system DejaVu font — skipping");
        return;
    }
    Shaper shaper;
    const uint32_t id = shaper.loadFont(fontPath).value();
    std::vector<ShapedGlyph> glyphs;

    // LTR base ending in an RTL run + trailing spaces (clusters 12, 13):
    // the spaces stay at the line's right end, after the Arabic block.
    REQUIRE(shaper.shape(id, 16.0f, "hej سلام  ", &glyphs));
    REQUIRE(glyphs.size() >= 8);
    REQUIRE(glyphs[glyphs.size() - 2].cluster == 12);
    REQUIRE(glyphs.back().cluster == 13);

    // RTL base with a trailing space (cluster 12): base level puts it at
    // the line's *left* end — first in the visual stream.
    REQUIRE(shaper.shape(id, 16.0f, "سلام abc ", &glyphs));
    REQUIRE(glyphs.front().cluster == 12);

    // The case only L1 gets right: inside an explicit RLE…PDF embedding
    // ("abc ‫سلام ‬"), the space before the terminator (cluster
    // 15) carries the embedding's RTL level through the implicit rules —
    // L1 resets it to the base level at end of line, so it renders after
    // the Arabic block instead of to the left of it.
    REQUIRE(shaper.shape(id, 16.0f, "abc ‫سلام ‬", &glyphs));
    size_t spaceAt = glyphs.size();
    size_t lastArabic = 0;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        if (glyphs[i].cluster == 15) spaceAt = i;
        if (glyphs[i].cluster >= 7 && glyphs[i].cluster <= 14) lastArabic = i;
    }
    REQUIRE(spaceAt < glyphs.size()); // the space glyph exists
    REQUIRE(spaceAt > lastArabic);
}
