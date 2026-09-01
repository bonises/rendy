#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "scene/animation_sampler.hpp"

using Catch::Approx;
using namespace rendy;
using detail::advanceWeight;
using detail::AnimationChannel;
using detail::animationSampleTime;
using detail::sampleAnimationChannel;
using detail::TransformAccumulator;

namespace {

AnimationChannel makeChannel(AnimationChannel::Path path,
                             AnimationChannel::Interpolation interpolation,
                             std::vector<float> times, std::vector<Vec4> values) {
    AnimationChannel channel;
    channel.path = path;
    channel.interpolation = interpolation;
    channel.times = std::move(times);
    channel.values = std::move(values);
    return channel;
}

} // namespace

TEST_CASE("linear translation interpolates and clamps", "[scene][animation]") {
    auto channel = makeChannel(AnimationChannel::Path::Translation,
                               AnimationChannel::Interpolation::Linear, {0.0f, 2.0f},
                               {{0, 0, 0, 0}, {10, 0, 0, 0}});
    REQUIRE(sampleAnimationChannel(channel, -1.0f).x == Approx(0.0f)); // clamp before
    REQUIRE(sampleAnimationChannel(channel, 1.0f).x == Approx(5.0f));  // midpoint
    REQUIRE(sampleAnimationChannel(channel, 5.0f).x == Approx(10.0f)); // clamp after
}

TEST_CASE("step holds the previous keyframe", "[scene][animation]") {
    auto channel = makeChannel(AnimationChannel::Path::Scale,
                               AnimationChannel::Interpolation::Step, {0.0f, 1.0f},
                               {{1, 1, 1, 0}, {2, 2, 2, 0}});
    REQUIRE(sampleAnimationChannel(channel, 0.99f).x == Approx(1.0f));
    REQUIRE(sampleAnimationChannel(channel, 1.0f).x == Approx(2.0f));
}

TEST_CASE("rotation uses slerp and stays normalized", "[scene][animation]") {
    // Identity → 90° around Z.
    const Quat q90 = glm::angleAxis(HalfPi, Vec3{0, 0, 1});
    auto channel = makeChannel(AnimationChannel::Path::Rotation,
                               AnimationChannel::Interpolation::Linear, {0.0f, 1.0f},
                               {{0, 0, 0, 1}, {q90.x, q90.y, q90.z, q90.w}});
    const Vec4 mid = sampleAnimationChannel(channel, 0.5f);
    const Quat q{mid.w, mid.x, mid.y, mid.z};
    REQUIRE(glm::length(q) == Approx(1.0f).margin(1e-5));
    // Halfway = 45° around Z.
    const Vec3 rotated = q * Vec3{1, 0, 0};
    REQUIRE(rotated.x == Approx(std::cos(Pi / 4)).margin(1e-4));
    REQUIRE(rotated.y == Approx(std::sin(Pi / 4)).margin(1e-4));
}

TEST_CASE("cubic spline hits keyframe values exactly", "[scene][animation]") {
    // Two keys with zero tangents behave like smoothstep between values.
    auto channel = makeChannel(
        AnimationChannel::Path::Translation, AnimationChannel::Interpolation::CubicSpline,
        {0.0f, 1.0f},
        {// key 0: inTangent, value, outTangent
         {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
         // key 1
         {0, 0, 0, 0}, {4, 0, 0, 0}, {0, 0, 0, 0}});
    REQUIRE(sampleAnimationChannel(channel, 0.0f).x == Approx(0.0f));
    REQUIRE(sampleAnimationChannel(channel, 1.0f).x == Approx(4.0f));
    REQUIRE(sampleAnimationChannel(channel, 0.5f).x == Approx(2.0f)); // symmetric
    // Smoothstep: slow start.
    REQUIRE(sampleAnimationChannel(channel, 0.25f).x < 1.0f);
}

TEST_CASE("clips that do not start at t=0 play their whole window",
          "[scene][animation]") {
    // Codex review finding 3: keys at [2, 3] must animate over 1 s, not 3.
    float sampleTime = 0.0f;
    // Looping: playback 0 → sample at clip start.
    REQUIRE(animationSampleTime(0.0f, 2.0f, 1.0f, true, &sampleTime));
    REQUIRE(sampleTime == Approx(2.0f));
    // Playback 0.5 → halfway through the window.
    REQUIRE(animationSampleTime(0.5f, 2.0f, 1.0f, true, &sampleTime));
    REQUIRE(sampleTime == Approx(2.5f));
    // Loops with the 1 s period, not 3 s.
    REQUIRE(animationSampleTime(1.25f, 2.0f, 1.0f, true, &sampleTime));
    REQUIRE(sampleTime == Approx(2.25f));

    // Non-looping: finishes at the end and reports done.
    REQUIRE_FALSE(animationSampleTime(1.5f, 2.0f, 1.0f, false, &sampleTime));
    REQUIRE(sampleTime == Approx(3.0f));
    // Negative playback (reverse/underflow) clamps at the start.
    REQUIRE_FALSE(animationSampleTime(-0.5f, 2.0f, 1.0f, false, &sampleTime));
    REQUIRE(sampleTime == Approx(2.0f));
}

TEST_CASE("blend accumulator averages with normalized weights",
          "[scene][animation][blend]") {
    // Two clips whose weights sum to 1: pure weighted average.
    TransformAccumulator accumulator;
    accumulator.add(AnimationChannel::Path::Translation, {0, 0, 0, 0}, 0.75f);
    accumulator.add(AnimationChannel::Path::Translation, {10, 0, 0, 0}, 0.25f);
    Transform transform;
    accumulator.apply(transform, Transform{});
    REQUIRE(transform.position.x == Approx(2.5f));

    // Weights above 1 normalize.
    TransformAccumulator heavy;
    heavy.add(AnimationChannel::Path::Translation, {10, 0, 0, 0}, 1.0f);
    heavy.add(AnimationChannel::Path::Translation, {20, 0, 0, 0}, 1.0f);
    Transform heavyTransform;
    heavy.apply(heavyTransform, Transform{});
    REQUIRE(heavyTransform.position.x == Approx(15.0f));

    // A single clip below weight 1 blends toward the BASE pose (Codex round
    // 2, finding 2: a fading sparse channel must ease back, not freeze).
    TransformAccumulator solo;
    solo.add(AnimationChannel::Path::Scale, {2, 2, 2, 0}, 0.25f);
    Transform base;
    base.scale = Vec3{1.0f};
    Transform soloTransform;
    solo.apply(soloTransform, base);
    REQUIRE(soloTransform.scale.x == Approx(0.25f * 2.0f + 0.75f * 1.0f));

    // Untouched channels keep their existing values.
    REQUIRE(soloTransform.position.x == Approx(0.0f));
    REQUIRE(soloTransform.rotation.w == Approx(1.0f));
}

TEST_CASE("sparse channel fade-out eases to the base pose",
          "[scene][animation][blend]") {
    // Clip A animates a node's translation; clip B (the crossfade target)
    // has no channel there. As A's weight → 0, the node returns to base.
    const Vec4 animated{10, 0, 0, 0};
    Transform base;
    base.position = Vec3{2, 0, 0};

    float previous = 10.0f;
    for (float weight : {0.75f, 0.5f, 0.25f, 0.05f}) {
        TransformAccumulator accumulator;
        accumulator.add(AnimationChannel::Path::Translation, animated, weight);
        Transform transform;
        accumulator.apply(transform, base);
        const float expected = weight * 10.0f + (1.0f - weight) * 2.0f;
        REQUIRE(transform.position.x == Approx(expected));
        REQUIRE(transform.position.x < previous); // strictly easing toward 2
        previous = transform.position.x;
    }
}

TEST_CASE("quaternion blending is hemisphere-safe and normalized",
          "[scene][animation][blend]") {
    const Quat q90 = glm::angleAxis(HalfPi, Vec3{0, 0, 1});
    TransformAccumulator accumulator;
    accumulator.add(AnimationChannel::Path::Rotation, {0, 0, 0, 1}, 0.5f);
    // Same 90° rotation but negated quaternion (other hemisphere).
    accumulator.add(AnimationChannel::Path::Rotation, {-q90.x, -q90.y, -q90.z, -q90.w}, 0.5f);
    Transform transform;
    accumulator.apply(transform, Transform{});
    REQUIRE(glm::length(transform.rotation) == Approx(1.0f).margin(1e-5));
    // Result should be ~45° around Z, not a degenerate mix.
    const Vec3 rotated = transform.rotation * Vec3{1, 0, 0};
    REQUIRE(rotated.y == Approx(std::sin(Pi / 4)).margin(0.05));
}

TEST_CASE("advanceWeight moves toward the target and stops there",
          "[scene][animation][blend]") {
    float weight = 0.0f;
    weight = advanceWeight(weight, 1.0f, 2.0f, 0.25f); // +0.5
    REQUIRE(weight == Approx(0.5f));
    weight = advanceWeight(weight, 1.0f, 2.0f, 1.0f); // overshoot clamps
    REQUIRE(weight == Approx(1.0f));
    weight = advanceWeight(weight, 0.0f, 4.0f, 0.125f); // fading out
    REQUIRE(weight == Approx(0.5f));
}
