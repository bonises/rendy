#pragma once

// CPU-side 2D batch state. Pure data — no Vulkan here; Renderer2D uploads
// and draws it. Layout of Quad2D must match shaders/quad2d.* exactly.

#include "rendy/math/math.hpp"

#include <cstdint>
#include <vector>

namespace rendy::text {
class GlyphCache;
}

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
inline constexpr float kQuadKindShadow = 3.0f; // info.y = blur radius px

struct CanvasData {
    text::GlyphCache* glyphCache = nullptr;
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

/// A replayable slice of a frame's batch (damage-based 2D: the UI records
/// its quads once and replays them until something changes). Clip indices
/// are stored rebased: 0 = the frame's viewport clip, >= 1 = 1 + index into
/// `clips`.
struct CanvasSnapshot {
    std::vector<Quad2D> quads;
    std::vector<Vec4> clips;
};

/// Captures everything drawn after (quadBase, clipBase) into `out`.
/// Returns false — and leaves `out` empty — when a quad references a
/// pre-existing clip other than the viewport (index 0): such references
/// can't be rebased into a later frame.
inline bool captureSnapshot(const CanvasData& data, size_t quadBase, size_t clipBase,
                            CanvasSnapshot* out) {
    out->quads.clear();
    out->clips.clear();
    out->quads.reserve(data.quads.size() - quadBase);
    for (size_t i = quadBase; i < data.quads.size(); ++i) {
        Quad2D quad = data.quads[i];
        const auto clip = static_cast<size_t>(quad.info.z);
        if (clip == 0) {
            // stays 0
        } else if (clip >= clipBase) {
            quad.info.z = static_cast<float>(clip - clipBase + 1);
        } else {
            out->quads.clear();
            return false;
        }
        out->quads.push_back(quad);
    }
    out->clips.assign(data.clips.begin() + static_cast<long>(clipBase), data.clips.end());
    return true;
}

/// Appends a snapshot to the current frame, remapping clip indices.
inline void replaySnapshot(const CanvasSnapshot& snapshot, CanvasData* data) {
    const auto newBase = static_cast<uint32_t>(data->clips.size());
    data->clips.insert(data->clips.end(), snapshot.clips.begin(), snapshot.clips.end());
    data->quads.reserve(data->quads.size() + snapshot.quads.size());
    for (Quad2D quad : snapshot.quads) {
        if (quad.info.z != 0.0f)
            quad.info.z = static_cast<float>(quad.info.z - 1.0f) + static_cast<float>(newBase);
        data->quads.push_back(quad);
    }
}

} // namespace rendy::detail
