#pragma once

/// \file mesh.hpp
/// CPU-side mesh data and GPU mesh handles.

#include "../math/math.hpp"

#include <cstdint>
#include <vector>

namespace rendy {

struct Vertex {
    Vec3 position{0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    Vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f}; ///< xyz tangent, w handedness
    Vec2 uv{0.0f};
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    /// Bounding sphere for culling; computed by Scene if radius == 0.
    Vec3 boundsCenter{0.0f};
    float boundsRadius = 0.0f;
};

struct MeshHandle {
    uint32_t id = UINT32_MAX;
    [[nodiscard]] bool valid() const { return id != UINT32_MAX; }
};

struct MaterialHandle {
    uint32_t id = 0; ///< 0 = default material
};

} // namespace rendy
