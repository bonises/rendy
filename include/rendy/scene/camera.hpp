#pragma once

/// \file camera.hpp

#include "../math/math.hpp"

namespace rendy {

struct Camera {
    Vec3 position{0.0f, 2.0f, 5.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float fovY = radians(60.0f);
    float nearPlane = 0.1f;
    float farPlane = 300.0f;

    void lookAt(Vec3 eye, Vec3 target, Vec3 up = {0.0f, 1.0f, 0.0f}) {
        position = eye;
        rotation = glm::quatLookAt(glm::normalize(target - eye), up);
    }

    [[nodiscard]] Vec3 forward() const { return rotation * Vec3{0.0f, 0.0f, -1.0f}; }
    [[nodiscard]] Vec3 right() const { return rotation * Vec3{1.0f, 0.0f, 0.0f}; }
    [[nodiscard]] Vec3 up() const { return rotation * Vec3{0.0f, 1.0f, 0.0f}; }

    [[nodiscard]] Mat4 view() const {
        return glm::lookAt(position, position + forward(), up());
    }
    [[nodiscard]] Mat4 proj(float aspect) const {
        Mat4 p = glm::perspective(fovY, aspect, nearPlane, farPlane);
        p[1][1] *= -1.0f; // GL → Vulkan clip space
        return p;
    }
};

} // namespace rendy
