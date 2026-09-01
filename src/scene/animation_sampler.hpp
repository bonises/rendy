#pragma once

// Animation channel data + keyframe sampling. Header-only and GPU-free so
// the interpolation math is unit-testable.

#include "rendy/math/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rendy::detail {

struct AnimationChannel {
    enum class Path : uint8_t { Translation, Rotation, Scale };
    enum class Interpolation : uint8_t { Step, Linear, CubicSpline };
    uint32_t node = UINT32_MAX; // scene node index
    Path path = Path::Translation;
    Interpolation interpolation = Interpolation::Linear;
    std::vector<float> times;
    // vec3 payloads in xyz; rotations as quaternion xyzw. CubicSpline stores
    // triples per keyframe: inTangent, value, outTangent.
    std::vector<Vec4> values;
};

/// Samples one channel at absolute clip time `t` (caller handles looping).
/// Rotations return quaternion xyzw; vec3 payloads use xyz.
inline Vec4 sampleAnimationChannel(const AnimationChannel& channel, float t) {
    using Interp = AnimationChannel::Interpolation;
    const auto& times = channel.times;
    const bool cubic = channel.interpolation == Interp::CubicSpline;
    const size_t stride = cubic ? 3 : 1; // inTangent, value, outTangent
    auto valueAt = [&](size_t key) { return channel.values[key * stride + (cubic ? 1 : 0)]; };

    if (times.empty()) return Vec4{0.0f};
    if (t <= times.front()) return valueAt(0);
    if (t >= times.back()) return valueAt(times.size() - 1);

    const auto it = std::upper_bound(times.begin(), times.end(), t);
    const size_t next = static_cast<size_t>(it - times.begin());
    const size_t prev = next - 1;
    const float span = times[next] - times[prev];
    const float u = span > 0.0f ? (t - times[prev]) / span : 0.0f;

    switch (channel.interpolation) {
    case Interp::Step:
        return valueAt(prev);
    case Interp::Linear:
        if (channel.path == AnimationChannel::Path::Rotation) {
            const Vec4 a = valueAt(prev);
            const Vec4 b = valueAt(next);
            const Quat qa{a.w, a.x, a.y, a.z};
            const Quat qb{b.w, b.x, b.y, b.z};
            const Quat q = glm::slerp(qa, qb, u);
            return {q.x, q.y, q.z, q.w};
        }
        return glm::mix(valueAt(prev), valueAt(next), u);
    case Interp::CubicSpline: {
        // glTF Hermite: p(u) = h00 v0 + h10 d b0 + h01 v1 + h11 d a1
        const Vec4 v0 = channel.values[prev * 3 + 1];
        const Vec4 b0 = channel.values[prev * 3 + 2]; // out-tangent of prev
        const Vec4 a1 = channel.values[next * 3 + 0]; // in-tangent of next
        const Vec4 v1 = channel.values[next * 3 + 1];
        const float u2 = u * u;
        const float u3 = u2 * u;
        Vec4 result = (2.0f * u3 - 3.0f * u2 + 1.0f) * v0 +
                      (u3 - 2.0f * u2 + u) * span * b0 +
                      (-2.0f * u3 + 3.0f * u2) * v1 + (u3 - u2) * span * a1;
        if (channel.path == AnimationChannel::Path::Rotation) {
            const Quat q = glm::normalize(Quat{result.w, result.x, result.y, result.z});
            result = {q.x, q.y, q.z, q.w};
        }
        return result;
    }
    }
    return valueAt(prev);
}

/// Maps playback time (seconds since play, scaled by speed) to a clip-local
/// sample time, honoring clips whose first keyframe isn't at 0. Returns
/// false when a non-looping clip has finished.
inline bool animationSampleTime(float playbackTime, float startTime, float duration,
                                bool loop, float* outTime) {
    if (duration <= 0.0f) {
        *outTime = startTime;
        return false; // static pose: apply once, then stop
    }
    float local = playbackTime;
    if (loop) {
        local = std::fmod(local, duration);
        if (local < 0.0f) local += duration;
    } else if (local >= duration) {
        *outTime = startTime + duration;
        return false;
    } else if (local < 0.0f) {
        *outTime = startTime;
        return false;
    }
    *outTime = startTime + local;
    return true;
}

/// Accumulates weighted TRS samples from several clips for one node and
/// resolves them into a Transform (weights are normalized, so a single clip
/// at weight 0.3 still gives its full pose).
struct TransformAccumulator {
    Vec3 translation{0.0f};
    float translationWeight = 0.0f;
    Vec4 rotation{0.0f}; // accumulated quat, hemisphere-corrected
    float rotationWeight = 0.0f;
    Vec3 scale{0.0f};
    float scaleWeight = 0.0f;

    void add(AnimationChannel::Path path, const Vec4& value, float weight) {
        switch (path) {
        case AnimationChannel::Path::Translation:
            translation += Vec3(value) * weight;
            translationWeight += weight;
            break;
        case AnimationChannel::Path::Rotation: {
            // Keep quaternions in the same hemisphere before averaging.
            Vec4 q = value;
            if (rotationWeight > 0.0f && glm::dot(q, rotation) < 0.0f) q = -q;
            rotation += q * weight;
            rotationWeight += weight;
            break;
        }
        case AnimationChannel::Path::Scale:
            scale += Vec3(value) * weight;
            scaleWeight += weight;
            break;
        }
    }

    /// Writes the blended result onto `transform` (untouched channels keep
    /// their current values).
    void apply(Transform& transform) const {
        if (translationWeight > 1e-5f) transform.position = translation / translationWeight;
        if (rotationWeight > 1e-5f) {
            const Vec4 q = rotation / rotationWeight;
            const Quat quat{q.w, q.x, q.y, q.z};
            const float length = glm::length(quat);
            if (length > 1e-5f) transform.rotation = quat / length;
        }
        if (scaleWeight > 1e-5f) transform.scale = scale / scaleWeight;
    }
};

/// Moves `weight` toward `target` by at most rate*dt. Returns the new value.
inline float advanceWeight(float weight, float target, float rate, float dt) {
    const float delta = target - weight;
    const float step = rate * dt;
    if (std::abs(delta) <= step) return target;
    return weight + (delta > 0.0f ? step : -step);
}

} // namespace rendy::detail
