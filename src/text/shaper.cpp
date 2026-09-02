#include "text/shaper.hpp"

#include "rendy/core/log.hpp" // formatted err()
#include "text/utf8.hpp"

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

/// Splits a line into maximal same-direction runs. Mixed lines (e.g. Latin
/// with an embedded Arabic phrase) must shape per run: HarfBuzz needs one
/// direction per buffer, and an RTL run's glyphs come back visually
/// ordered. Runs lay out in logical order — correct for the common
/// LTR-base case; full bidi reordering is out of scope for v1.
void splitDirectionRuns(std::string_view utf8, std::vector<Shaper::DirectionRun>* runs) {
    runs->clear();
    hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();
    Shaper::DirectionRun current;
    bool haveStrong = false;
    size_t offset = 0;
    while (offset < utf8.size()) {
        const size_t at = offset;
        const uint32_t codepoint = decodeUtf8(utf8, offset);
        // Spaces/punctuation/digits are COMMON script — direction-neutral
        // (hb reports LTR for them, which would chop an RTL phrase into
        // per-word runs and scramble the word order).
        const hb_script_t script = hb_unicode_script(unicode, codepoint);
        const bool neutral = script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED ||
                             script == HB_SCRIPT_UNKNOWN;
        const hb_direction_t direction =
            neutral ? HB_DIRECTION_INVALID : hb_script_get_horizontal_direction(script);
        const bool strong = direction == HB_DIRECTION_LTR || direction == HB_DIRECTION_RTL;
        const bool rtl = direction == HB_DIRECTION_RTL;
        if (strong && haveStrong && rtl != current.rtl) {
            current.end = at;
            runs->push_back(current);
            current = {at, at, rtl};
        } else if (strong && !haveStrong) {
            current.rtl = rtl; // leading neutrals join this run
            haveStrong = true;
        }
    }
    current.end = utf8.size();
    if (current.end > current.start) runs->push_back(current);
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

    splitDirectionRuns(utf8, &runs_);
    for (const DirectionRun& run : runs_) {
        const hb_direction_t direction = run.rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR;
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
            out->push_back(glyph);
        }
    }
    return true;
}

} // namespace rendy::text
