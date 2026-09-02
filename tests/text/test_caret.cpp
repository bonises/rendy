#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "text/caret.hpp"
#include "text/shaper.hpp"

#include <cstdint>
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

TEST_CASE("selection rects: one interval in single-direction text", "[text][caret]") {
    const char* fontPath = dejaVu();
    if (fontPath == nullptr) {
        SUCCEED("no system DejaVu font — skipping");
        return;
    }
    Shaper shaper;
    const uint32_t id = shaper.loadFont(fontPath).value();
    std::vector<ShapedGlyph> glyphs;
    std::vector<std::pair<float, float>> rects;

    // LTR: exactly the caretX span, as one interval.
    const std::string_view latin = "hello";
    REQUIRE(shaper.shape(id, 16.0f, latin, &glyphs));
    selectionRects(glyphs, latin, 1, 4, &rects);
    REQUIRE(rects.size() == 1);
    REQUIRE(rects[0].first == Approx(caretX(glyphs, latin, 1)));
    REQUIRE(rects[0].second == Approx(caretX(glyphs, latin, 4)));

    // RTL: still one interval, mirrored (later bytes sit further left).
    const std::string_view arabic = "سلام";
    REQUIRE(shaper.shape(id, 16.0f, arabic, &glyphs));
    selectionRects(glyphs, arabic, 2, 6, &rects);
    REQUIRE(rects.size() == 1);
    REQUIRE(rects[0].first == Approx(caretX(glyphs, arabic, 6)));
    REQUIRE(rects[0].second == Approx(caretX(glyphs, arabic, 2)));

    // Degenerate ranges: empty.
    selectionRects(glyphs, arabic, 3, 3, &rects);
    REQUIRE(rects.empty());
    selectionRects(glyphs, arabic, 6, 2, &rects);
    REQUIRE(rects.empty());
}

TEST_CASE("selection rects: mixed-direction selections are disjoint", "[text][caret]") {
    const char* fontPath = dejaVu();
    if (fontPath == nullptr) {
        SUCCEED("no system DejaVu font — skipping");
        return;
    }
    Shaper shaper;
    const uint32_t id = shaper.loadFont(fontPath).value();

    // "hej سلام عليكم igen": bytes hej=0..2, sp 3, سلام=4..11, sp 12,
    // عليكم=13..22, sp 23, igen=24..27. Visually the Arabic block is
    // reversed: [عليكم سلام]. Selecting logically from inside "hej" into
    // سلام covers pieces that are NOT visual neighbours (the end of hej at
    // the left, سلام at the block's right edge) — two rects with the
    // unselected عليكم in the gap between them.
    const std::string_view line = "hej سلام عليكم igen";
    std::vector<ShapedGlyph> glyphs;
    REQUIRE(shaper.shape(id, 16.0f, line, &glyphs));
    std::vector<std::pair<float, float>> rects;
    selectionRects(glyphs, line, 1, 8, &rects); // "ej " + first half of سلام
    REQUIRE(rects.size() == 2);
    REQUIRE(rects[0].second < rects[1].first); // sorted, truly disjoint

    // Partition check across every codepoint: each character's own
    // highlight centre must fall inside the range's rects exactly when the
    // character is selected. (The old single-rect approximation failed
    // this — unselected عليكم sat inside the big rect.)
    const auto covered = [&](float x) {
        for (const auto& [x0, x1] : rects)
            if (x >= x0 - 0.01f && x <= x1 + 0.01f) return true;
        return false;
    };
    std::vector<std::pair<float, float>> charRect;
    size_t offset = 0;
    while (offset < line.size()) {
        const size_t at = offset;
        decodeUtf8(line, offset); // advances to the next codepoint
        selectionRects(glyphs, line, at, offset, &charRect);
        REQUIRE(charRect.size() == 1);
        const float centre = 0.5f * (charRect[0].first + charRect[0].second);
        const bool selected = at >= 1 && at < 8;
        REQUIRE(covered(centre) == selected);
    }

    // The whole line selected merges back into one interval.
    selectionRects(glyphs, line, 0, line.size(), &rects);
    REQUIRE(rects.size() == 1);
    REQUIRE(rects[0].first == Approx(0.0f).margin(0.01));
    REQUIRE(rects[0].second == Approx(width(glyphs)).margin(0.01));
}
