#include "rendy/canvas/canvas.hpp"

#include "canvas/canvas_data.hpp"
#include "text/glyph_cache.hpp"
#include "text/utf8.hpp"

#include <algorithm>
#include <cmath>

namespace rendy {

using detail::CanvasData;
using detail::Quad2D;

void Canvas::drawRect(const Rect& rect, const DrawRectOptions& options) {
    if (rect.empty()) return;
    if (options.shadowBlur > 0.0f && options.shadowColor.a > 0.0f) {
        // Shadow quad: the rect offset and expanded by the blur radius; the
        // shader evaluates the SDF against the un-expanded box.
        const float blur = options.shadowBlur;
        Quad2D shadow{};
        shadow.rect = {rect.pos.x + options.shadowOffset.x - blur,
                       rect.pos.y + options.shadowOffset.y - blur,
                       rect.size.x + 2.0f * blur, rect.size.y + 2.0f * blur};
        shadow.uvRect = {0.0f, 0.0f, 1.0f, 1.0f};
        shadow.color = {options.shadowColor.r, options.shadowColor.g, options.shadowColor.b,
                        options.shadowColor.a};
        shadow.radii = options.cornerRadii.x >= 0.0f ? options.cornerRadii
                                                     : Vec4{options.cornerRadius};
        shadow.info = {0.0f, blur, static_cast<float>(data_->currentClip()),
                       detail::kQuadKindShadow};
        data_->quads.push_back(shadow);
    }
    Quad2D quad{};
    quad.rect = {rect.pos.x, rect.pos.y, rect.size.x, rect.size.y};
    quad.uvRect = {0.0f, 0.0f, 1.0f, 1.0f};
    quad.color = {options.color.r, options.color.g, options.color.b, options.color.a};
    quad.radii = options.cornerRadii.x >= 0.0f ? options.cornerRadii
                                               : Vec4{options.cornerRadius};
    quad.borderColor = {options.borderColor.r, options.borderColor.g, options.borderColor.b,
                        options.borderColor.a};
    quad.info = {options.borderWidth, 0.0f, static_cast<float>(data_->currentClip()),
                 detail::kQuadKindSolid};
    data_->quads.push_back(quad);
}

void Canvas::drawImage(TextureRef texture, const Rect& rect, const DrawImageOptions& options) {
    if (rect.empty()) return;
    Quad2D quad{};
    quad.rect = {rect.pos.x, rect.pos.y, rect.size.x, rect.size.y};
    quad.uvRect = {options.uv.left(), options.uv.top(), options.uv.right(),
                   options.uv.bottom()};
    quad.color = {options.tint.r, options.tint.g, options.tint.b, options.tint.a};
    quad.radii = Vec4{options.cornerRadius};
    quad.borderColor = Vec4{0.0f};
    quad.info = {0.0f, static_cast<float>(texture.index),
                 static_cast<float>(data_->currentClip()), detail::kQuadKindImage};
    data_->quads.push_back(quad);
}

namespace {

// Shared walk for drawText/measureText: HarfBuzz shaping per line —
// ligatures, kerning and complex scripts come from the shaper; the glyph
// cache rasterizes the resulting glyph ids on demand.
template <typename PerGlyph>
Vec2 layoutText(text::GlyphCache& cache, std::string_view str, Vec2 pos, FontRef font,
                float size, PerGlyph&& perGlyph) {
    const TextMetrics metrics = cache.metrics(font.id, size);
    const float startX = std::round(pos.x);
    float baseline = std::round(pos.y + metrics.ascent);
    float maxX = startX;
    int lines = 0;
    size_t lineStart = 0;
    while (lineStart <= str.size()) {
        const size_t newline = str.find('\n', lineStart);
        const std::string_view line = str.substr(
            lineStart,
            (newline == std::string_view::npos ? str.size() : newline) - lineStart);
        lines++;
        float x = startX;
        for (const text::ShapedGlyph& shaped : cache.shapeLine(font.id, size, line)) {
            const text::GlyphInfo* glyph = cache.glyph(font.id, size, shaped.glyphIndex);
            if (glyph != nullptr)
                perGlyph(*glyph, x + shaped.xOffset, baseline - shaped.yOffset);
            x += shaped.xAdvance;
        }
        maxX = std::max(maxX, x);
        if (newline == std::string_view::npos) break;
        lineStart = newline + 1;
        baseline += metrics.lineHeight;
    }
    return {maxX - startX, static_cast<float>(lines) * metrics.lineHeight};
}

} // namespace

Vec2 Canvas::drawText(std::string_view str, Vec2 pos, const DrawTextOptions& options) {
    text::GlyphCache* cache = data_->glyphCache;
    if (cache == nullptr || !cache->hasFont(options.font.id)) return {0.0f, 0.0f};
    const Vec4 color{options.color.r, options.color.g, options.color.b, options.color.a};
    const float clip = static_cast<float>(data_->currentClip());
    return layoutText(*cache, str, pos, options.font, options.size,
                      [&](const text::GlyphInfo& g, float x, float baseline) {
                          if (!g.hasPixels) return;
                          Quad2D quad{};
                          quad.rect = {std::round(x + g.bearing.x), std::round(baseline - g.bearing.y),
                                       g.size.x, g.size.y};
                          quad.uvRect = {g.uvMin.x, g.uvMin.y, g.uvMax.x, g.uvMax.y};
                          quad.color = color;
                          quad.radii = Vec4{0.0f};
                          quad.borderColor = Vec4{0.0f};
                          quad.info = {0.0f, static_cast<float>(g.textureIndex), clip,
                                       detail::kQuadKindText};
                          data_->quads.push_back(quad);
                      });
}

Vec2 Canvas::measureText(std::string_view str, const DrawTextOptions& options) {
    text::GlyphCache* cache = data_->glyphCache;
    if (cache == nullptr || !cache->hasFont(options.font.id)) return {0.0f, 0.0f};
    return layoutText(*cache, str, {0.0f, 0.0f}, options.font, options.size,
                      [](const text::GlyphInfo&, float, float) {});
}

namespace {

// Greedy word wrap; a word longer than maxWidth breaks inside itself.
// `measure` measures a substring's width; `emit` receives each line.
template <typename Measure, typename Emit>
void wrapLines(std::string_view str, float maxWidth, Measure&& measure, Emit&& emit) {
    size_t paragraphStart = 0;
    while (paragraphStart <= str.size()) {
        size_t newline = str.find('\n', paragraphStart);
        const std::string_view paragraph =
            str.substr(paragraphStart,
                       (newline == std::string_view::npos ? str.size() : newline) -
                           paragraphStart);

        if (paragraph.empty()) {
            emit(std::string_view{});
        } else {
            size_t lineStart = 0;
            while (lineStart < paragraph.size()) {
                // Extend the line word by word while it fits.
                size_t lineEnd = lineStart;
                size_t cursor = lineStart;
                while (cursor <= paragraph.size()) {
                    size_t wordEnd = paragraph.find(' ', cursor);
                    if (wordEnd == std::string_view::npos) wordEnd = paragraph.size();
                    const auto candidate = paragraph.substr(lineStart, wordEnd - lineStart);
                    if (lineEnd == lineStart || measure(candidate) <= maxWidth) {
                        lineEnd = wordEnd;
                        cursor = wordEnd + 1;
                        if (wordEnd == paragraph.size()) break;
                    } else {
                        break;
                    }
                }

                auto line = paragraph.substr(lineStart, lineEnd - lineStart);
                if (measure(line) > maxWidth) {
                    // Single oversized word: break at codepoints (min 1).
                    size_t offset = 0;
                    size_t fit = 0;
                    while (offset < line.size()) {
                        const size_t prev = offset;
                        text::decodeUtf8(line, offset);
                        if (fit > 0 && measure(line.substr(0, offset)) > maxWidth) {
                            offset = prev;
                            break;
                        }
                        fit++;
                    }
                    line = line.substr(0, offset);
                    lineEnd = lineStart + offset;
                    emit(line);
                    lineStart = lineEnd; // no space to skip mid-word
                } else {
                    emit(line);
                    lineStart = lineEnd + 1; // skip the breaking space
                }
            }
        }

        if (newline == std::string_view::npos) break;
        paragraphStart = newline + 1;
    }
}

} // namespace

Vec2 Canvas::drawTextWrapped(std::string_view str, Vec2 pos, float maxWidth,
                             const DrawTextOptions& options) {
    text::GlyphCache* cache = data_->glyphCache;
    if (cache == nullptr || !cache->hasFont(options.font.id)) return {0.0f, 0.0f};
    const float lineHeight = cache->metrics(options.font.id, options.size).lineHeight;
    float y = pos.y;
    float widest = 0.0f;
    wrapLines(
        str, maxWidth,
        [&](std::string_view piece) { return measureText(piece, options).x; },
        [&](std::string_view line) {
            widest = std::max(widest, drawText(line, {pos.x, y}, options).x);
            y += lineHeight;
        });
    return {widest, y - pos.y};
}

Vec2 Canvas::measureTextWrapped(std::string_view str, float maxWidth,
                                const DrawTextOptions& options) {
    text::GlyphCache* cache = data_->glyphCache;
    if (cache == nullptr || !cache->hasFont(options.font.id)) return {0.0f, 0.0f};
    const float lineHeight = cache->metrics(options.font.id, options.size).lineHeight;
    float height = 0.0f;
    float widest = 0.0f;
    wrapLines(
        str, maxWidth,
        [&](std::string_view piece) { return measureText(piece, options).x; },
        [&](std::string_view line) {
            widest = std::max(widest, measureText(line, options).x);
            height += lineHeight;
        });
    return {widest, height};
}

TextMetrics Canvas::textMetrics(const DrawTextOptions& options) {
    text::GlyphCache* cache = data_->glyphCache;
    if (cache == nullptr || !cache->hasFont(options.font.id)) return {};
    return cache->metrics(options.font.id, options.size);
}

void Canvas::pushClip(const Rect& rect) {
    const Vec4 current = data_->clips[data_->currentClip()];
    const Rect currentRect{{current.x, current.y}, {current.z - current.x, current.w - current.y}};
    const Rect clipped = currentRect.intersect(rect);
    data_->clips.push_back(
        Vec4{clipped.left(), clipped.top(), clipped.right(), clipped.bottom()});
    data_->clipStack.push_back(static_cast<uint32_t>(data_->clips.size() - 1));
}

void Canvas::popClip() {
    if (data_->clipStack.size() > 1) data_->clipStack.pop_back();
}

Vec2 Canvas::size() const { return data_->viewport; }

} // namespace rendy
