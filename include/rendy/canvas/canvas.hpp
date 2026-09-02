#pragma once

/// \file canvas.hpp
/// Immediate-mode 2D drawing. Everything drawn in a frame is batched into a
/// single instanced draw call: solid rects, images, and text share one
/// pipeline. Submission order is paint order (painter's algorithm).

#include "../core/color.hpp"
#include "../core/rect.hpp"
#include "../gpu/texture.hpp"
#include "../math/math.hpp"
#include "font.hpp"

#include <string_view>

namespace rendy {

namespace detail {
struct CanvasData;
struct AppImpl;
} // namespace detail

namespace ui::detail {
struct ContextImpl;
} // namespace ui::detail

struct DrawRectOptions {
    Color color = colors::white;
    /// Uniform corner radius in px. For per-corner control set `cornerRadii`
    /// (tl, tr, br, bl); when any component is >= 0 it wins over cornerRadius.
    float cornerRadius = 0.0f;
    Vec4 cornerRadii{-1.0f};
    float borderWidth = 0.0f;
    Color borderColor = colors::black;
    /// Soft drop shadow behind the rect (blur 0 disables it). Costs one
    /// extra quad — same batch, no extra draw calls.
    float shadowBlur = 0.0f;
    Vec2 shadowOffset{0.0f, 2.0f};
    Color shadowColor{0.0f, 0.0f, 0.0f, 0.35f};
};

struct DrawImageOptions {
    Color tint = colors::white;
    /// Sub-rectangle of the texture in normalized UVs.
    Rect uv{{0.0f, 0.0f}, {1.0f, 1.0f}};
    float cornerRadius = 0.0f;
};

struct DrawTextOptions {
    FontRef font{}; ///< default: the app's default UI font
    float size = 16.0f;
    Color color = colors::white;
};

class Canvas {
public:
    void drawRect(const Rect& rect, const DrawRectOptions& options = {});
    void drawImage(TextureRef texture, const Rect& rect, const DrawImageOptions& options = {});

    /// Draws UTF-8 text with `pos` as the top-left of the first line
    /// ('\n' starts a new line). Returns the drawn size in px.
    Vec2 drawText(std::string_view text, Vec2 pos, const DrawTextOptions& options = {});
    /// Size drawText would occupy, without drawing.
    Vec2 measureText(std::string_view text, const DrawTextOptions& options = {});

    /// Like drawText but word-wraps at `maxWidth` px (falls back to breaking
    /// inside words that don't fit alone on a line).
    Vec2 drawTextWrapped(std::string_view text, Vec2 pos, float maxWidth,
                         const DrawTextOptions& options = {});
    Vec2 measureTextWrapped(std::string_view text, float maxWidth,
                            const DrawTextOptions& options = {});
    /// Vertical metrics for a font at a size (ascent/descent/lineHeight).
    TextMetrics textMetrics(const DrawTextOptions& options = {});

    /// Clip subsequent draws to `rect` (intersected with the current clip).
    void pushClip(const Rect& rect);
    void popClip();

    /// Canvas size in pixels this frame.
    [[nodiscard]] Vec2 size() const;

private:
    friend struct detail::AppImpl;
    friend struct ui::detail::ContextImpl; // damage-based paint caching
    explicit Canvas(detail::CanvasData* data) : data_(data) {}
    detail::CanvasData* data_;
};

} // namespace rendy
