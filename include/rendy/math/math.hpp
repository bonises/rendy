#pragma once

/// \file math.hpp
/// rendy's math vocabulary. Thin aliases over GLM so the whole GLM toolbox
/// works on rendy types, while the public API only ever says rendy::Vec3 etc.

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace rendy {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
using UVec2 = glm::uvec2;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Quat = glm::quat;

inline constexpr float Pi = 3.14159265358979323846f;
inline constexpr float TwoPi = 2.0f * Pi;
inline constexpr float HalfPi = 0.5f * Pi;

constexpr float radians(float degrees) { return degrees * (Pi / 180.0f); }
constexpr float degrees(float rad) { return rad * (180.0f / Pi); }

/// Position/rotation/scale, composed as T * R * S.
struct Transform {
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};

    [[nodiscard]] Mat4 matrix() const {
        Mat4 m = glm::mat4_cast(rotation);
        m[0] *= scale.x;
        m[1] *= scale.y;
        m[2] *= scale.z;
        m[3] = Vec4(position, 1.0f);
        return m;
    }
};

} // namespace rendy
