#pragma once

// HarfBuzz text shaping: Unicode text → positioned glyph indices, with
// ligatures, kerning and complex-script support (Arabic joining, Indic
// reordering). Uses hb-ot font functions on the raw font file — FreeType
// stays the rasterizer, so this class is GPU-free and unit-testable.
//
// Full bidi (UAX#9): SheenBidi computes embedding levels per line (base
// direction auto-detected from the first strong character, LTR default),
// each level run shapes through HarfBuzz in its own direction, and rule L2
// reorders the runs so the output glyph stream is in visual order.
// Script is guessed per run (hb_buffer_guess_segment_properties).

#include "rendy/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct hb_blob_t;
struct hb_face_t;
struct hb_font_t;
struct hb_buffer_t;

namespace rendy::text {

struct ShapedGlyph {
    uint32_t glyphIndex = 0; ///< font glyph id (0 = .notdef)
    uint32_t cluster = 0;    ///< byte offset of the source character(s)
    float xAdvance = 0.0f;   ///< px
    float xOffset = 0.0f;    ///< px, added to the pen position
    float yOffset = 0.0f;    ///< px, positive = up (font space)
    bool rtl = false;        ///< direction of the run this glyph shaped in
};

class Shaper {
public:
    Shaper();
    ~Shaper();

    Shaper(const Shaper&) = delete;
    Shaper& operator=(const Shaper&) = delete;

    /// Reads the font file; returns its slot id (kept in lockstep with the
    /// GlyphCache font ids by loading in the same order).
    Result<uint32_t> loadFont(const std::string& path);

    [[nodiscard]] bool hasFont(uint32_t fontId) const {
        return fontId < fonts_.size() && fonts_[fontId].font != nullptr;
    }

    /// Shapes one line (no '\n' handling) into positioned glyphs, appended
    /// to `out` (cleared first). Returns false for unknown fonts.
    bool shape(uint32_t fontId, float pixelSize, std::string_view utf8,
               std::vector<ShapedGlyph>* out);

    /// A maximal same-embedding-level slice of one line (internal, exposed
    /// for the run splitter). Odd levels are RTL.
    struct LevelRun {
        size_t start = 0;
        size_t end = 0;
        uint8_t level = 0;
    };

private:
    struct Font {
        hb_blob_t* blob = nullptr;
        hb_face_t* face = nullptr;
        hb_font_t* font = nullptr;
    };
    std::vector<Font> fonts_;
    hb_buffer_t* buffer_ = nullptr;
    std::vector<LevelRun> runs_;
};

} // namespace rendy::text
