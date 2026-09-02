#include "text/shaper.hpp"

#include "rendy/core/log.hpp" // formatted err()

#include <SheenBidi/SheenBidi.h>
#include <hb.h>

#include <algorithm>
#include <cmath>

namespace rendy::text {

Shaper::Shaper() { buffer_ = hb_buffer_create(); }

Shaper::~Shaper() {
    for (Font& font : fonts_) {
        if (font.font != nullptr) hb_font_destroy(font.font);
        if (font.face != nullptr) hb_face_destroy(font.face);
        if (font.blob != nullptr) hb_blob_destroy(font.blob);
    }
    if (buffer_ != nullptr) hb_buffer_destroy(buffer_);
}

Result<uint32_t> Shaper::loadFont(const std::string& path) {
    // Always claim a slot — ids stay in lockstep with the GlyphCache even
    // when a load fails (the slot is dead; hasFont() reports false).
    Font font;
    font.blob = hb_blob_create_from_file_or_fail(path.c_str());
    if (font.blob != nullptr) {
        font.face = hb_face_create(font.blob, 0);
        font.font = hb_font_create(font.face);
        // hb-ot font funcs are the default; no FreeType coupling needed.
    }
    fonts_.push_back(font);
    if (font.font == nullptr) return err("shaper: failed to read font '{}'", path);
    return static_cast<uint32_t>(fonts_.size() - 1);
}

namespace {

/// UAX#9 runs in visual (left-to-right screen) order via SheenBidi. The
/// base direction is auto-detected from the first strong character (LTR
/// when there is none). shape() receives one drawn line at a time, so
/// each paragraph is treated as a single SBLine — that runs rules L1–L2:
/// L1 resets trailing whitespace and segment separators to the base level
/// (visible with explicit embedding controls like RLE/PDF), L2 hands the
/// runs back already reordered. HarfBuzz needs the split anyway: one
/// direction per buffer, and an RTL run's glyphs come back visually
/// ordered within the run.
void visualRuns(std::string_view utf8, std::vector<Shaper::LevelRun>* runs) {
    runs->clear();
    const SBCodepointSequence sequence{SBStringEncodingUTF8, utf8.data(), utf8.size()};
    SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
    if (algorithm == nullptr) { // OOM: degrade to one LTR run
        runs->push_back({0, utf8.size(), 0});
        return;
    }
    size_t offset = 0;
    while (offset < utf8.size()) {
        // One paragraph per iteration; a line rarely holds separators
        // (shape() takes single lines) but U+2029 etc. would end one early.
        SBParagraphRef paragraph = SBAlgorithmCreateParagraph(
            algorithm, offset, utf8.size() - offset, SBLevelDefaultLTR);
        if (paragraph == nullptr) break;
        const SBUInteger length = SBParagraphGetLength(paragraph);
        if (length == 0) {
            SBParagraphRelease(paragraph);
            break;
        }
        if (SBLineRef line = SBParagraphCreateLine(paragraph, offset, length)) {
            const SBUInteger runCount = SBLineGetRunCount(line);
            const SBRun* lineRuns = SBLineGetRunsPtr(line);
            for (SBUInteger i = 0; i < runCount; ++i)
                runs->push_back({lineRuns[i].offset,
                                 lineRuns[i].offset + lineRuns[i].length,
                                 lineRuns[i].level});
            SBLineRelease(line);
        }
        SBParagraphRelease(paragraph);
        offset += length;
    }
    SBAlgorithmRelease(algorithm);
    if (runs->empty() && !utf8.empty()) runs->push_back({0, utf8.size(), 0});
}

} // namespace

bool Shaper::shape(uint32_t fontId, float pixelSize, std::string_view utf8,
                   std::vector<ShapedGlyph>* out) {
    out->clear();
    if (!hasFont(fontId) || utf8.empty()) return hasFont(fontId);
    Font& font = fonts_[fontId];

    // Positions come back in scale units: scale = size * 64 → px = units/64,
    // matching the FreeType 26.6 convention used elsewhere.
    const auto scale =
        static_cast<int>(std::lround(std::clamp(pixelSize, 1.0f, 512.0f) * 64.0f));
    hb_font_set_scale(font.font, scale, scale);

    visualRuns(utf8, &runs_);
    for (const LevelRun& run : runs_) {
        const bool rtl = (run.level & 1) != 0;
        const hb_direction_t direction = rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR;
        hb_buffer_clear_contents(buffer_);
        // Whole string + item range: the shaper sees the surrounding
        // context and clusters stay absolute byte offsets.
        hb_buffer_add_utf8(buffer_, utf8.data(), static_cast<int>(utf8.size()),
                           static_cast<unsigned>(run.start),
                           static_cast<int>(run.end - run.start));
        hb_buffer_set_direction(buffer_, direction);
        hb_buffer_guess_segment_properties(buffer_); // fills script + language
        hb_shape(font.font, buffer_, nullptr, 0);

        unsigned count = 0;
        const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer_, &count);
        const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer_, &count);
        out->reserve(out->size() + count);
        for (unsigned i = 0; i < count; ++i) {
            ShapedGlyph glyph;
            glyph.glyphIndex = infos[i].codepoint; // post-shaping: a glyph id
            glyph.cluster = infos[i].cluster;
            glyph.xAdvance = static_cast<float>(positions[i].x_advance) / 64.0f;
            glyph.xOffset = static_cast<float>(positions[i].x_offset) / 64.0f;
            glyph.yOffset = static_cast<float>(positions[i].y_offset) / 64.0f;
            glyph.rtl = rtl;
            out->push_back(glyph);
        }
    }
    return true;
}

} // namespace rendy::text
