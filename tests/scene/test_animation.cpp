#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "scene/animation_sampler.hpp"

using Catch::Approx;
using namespace rendy;
using detail::AnimationChannel;
using detail::animationSampleTime;
using detail::sampleAnimationChannel;

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
