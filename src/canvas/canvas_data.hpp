#pragma once

// CPU-side 2D batch state. Pure data — no Vulkan here; Renderer2D uploads
// and draws it. Layout of Quad2D must match shaders/quad2d.* exactly.

#include "rendy/math/math.hpp"

#include <vector>

namespace rendy::detail {

struct Quad2D {
    Vec4 rect;        // x, y, w, h px
    Vec4 uvRect;      // u0, v0, u1, v1
    Vec4 color;       // sRGB straight alpha
    Vec4 radii;       // tl, tr, br, bl px
    Vec4 borderColor; // sRGB
    Vec4 info;        // borderWidth, textureIndex, clipIndex, kind
};
static_assert(sizeof(Quad2D) == 96);

inline constexpr float kQuadKindSolid = 0.0f;
inline constexpr float kQuadKindImage = 1.0f;
inline constexpr float kQuadKindText = 2.0f;

struct CanvasData {
    std::vector<Quad2D> quads;
    std::vector<Vec4> clips;          // x0, y0, x1, y1
    std::vector<uint32_t> clipStack;  // indices into clips
    Vec2 viewport{0.0f};

    void reset(Vec2 viewportSize) {
        quads.clear();
        clips.clear();
        clipStack.clear();
        viewport = viewportSize;
        clips.push_back(Vec4{0.0f, 0.0f, viewportSize.x, viewportSize.y});
        clipStack.push_back(0);
    }

    [[nodiscard]] uint32_t currentClip() const { return clipStack.back(); }
};

} // namespace rendy::detail
