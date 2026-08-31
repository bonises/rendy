#pragma once

/// \file canvas.hpp
/// Immediate-mode 2D drawing. Everything drawn in a frame is batched into a
/// single instanced draw call: solid rects, images, and text share one
/// pipeline. Submission order is paint order (painter's algorithm).

#include "../core/color.hpp"
#include "../core/rect.hpp"
#include "../gpu/texture.hpp"
#include "../math/math.hpp"

namespace rendy {

namespace detail {
struct CanvasData;
struct AppImpl;
} // namespace detail

struct DrawRectOptions {
    Color color = colors::white;
    /// Uniform corner radius in px. For per-corner control set `cornerRadii`
    /// (tl, tr, br, bl); when any component is >= 0 it wins over cornerRadius.
    float cornerRadius = 0.0f;
    Vec4 cornerRadii{-1.0f};
    float borderWidth = 0.0f;
    Color borderColor = colors::black;
};

struct DrawImageOptions {
    Color tint = colors::white;
    /// Sub-rectangle of the texture in normalized UVs.
    Rect uv{{0.0f, 0.0f}, {1.0f, 1.0f}};
    float cornerRadius = 0.0f;
};

class Canvas {
public:
    void drawRect(const Rect& rect, const DrawRectOptions& options = {});
    void drawImage(TextureRef texture, const Rect& rect, const DrawImageOptions& options = {});

    /// Clip subsequent draws to `rect` (intersected with the current clip).
    void pushClip(const Rect& rect);
    void popClip();

    /// Canvas size in pixels this frame.
    [[nodiscard]] Vec2 size() const;

private:
    friend struct detail::AppImpl;
    explicit Canvas(detail::CanvasData* data) : data_(data) {}
    detail::CanvasData* data_;
};

} // namespace rendy
