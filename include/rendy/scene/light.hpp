#pragma once

/// \file light.hpp

#include "../core/color.hpp"
#include "../math/math.hpp"

#include <cstdint>

namespace rendy {

struct Light {
    enum class Type : uint8_t { Directional, Point, Spot };

    Type type = Type::Point;
    Vec3 position{0.0f};       ///< point/spot
    Vec3 direction{0.0f, -1.0f, 0.0f}; ///< directional/spot
    Color color = colors::white;
    /// Directional: illuminance-ish scale. Point/spot: intensity at 1 m.
    float intensity = 1.0f;
    /// Point/spot: light reaches zero here. 0 = unbounded.
    float range = 0.0f;
    float innerCone = radians(25.0f); ///< spot: full-intensity angle
    float outerCone = radians(35.0f); ///< spot: falloff-to-zero angle
    bool castsShadows = false;
};

} // namespace rendy
