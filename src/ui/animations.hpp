#pragma once

// CSS @keyframes animation runtime: timeline math (iterations, direction,
// fill) and keyframe tracks compiled to interpolable Vec4 keys. GPU-free
// (unit-testable); shares the easing/value machinery with transitions.hpp.

#include "css/cascade.hpp"
#include "css/stylesheet.hpp"
#include "ui/transitions.hpp"

#include <cmath>
#include <utility>
#include <vector>

namespace rendy::ui::anim {

/// Where an animation is on its timeline at `elapsed` seconds.
struct TimelineSample {
    bool active = false;   ///< a keyframe value should be applied
    bool finished = false; ///< ran out of iterations (never true for infinite)
    float progress = 0.0f; ///< 0..1 through the keyframes, direction applied
};

/// Maps elapsed time through delay, iteration count and direction. During
/// the delay nothing applies (no backwards fill); after the last iteration
/// the end pose applies only with fillForwards.
inline TimelineSample sampleTimeline(const AnimationSpec& spec, float elapsed) {
    TimelineSample sample;
    const float t = elapsed - spec.delay;
    if (t < 0.0f) return sample; // waiting out the delay

    const auto directed = [&](float local, float iteration) {
        bool reverse = spec.direction == AnimDirection::Reverse ||
                       spec.direction == AnimDirection::AlternateReverse;
        const bool alternate = spec.direction == AnimDirection::Alternate ||
                               spec.direction == AnimDirection::AlternateReverse;
        if (alternate && std::fmod(iteration, 2.0f) >= 1.0f) reverse = !reverse;
        return reverse ? 1.0f - local : local;
    };

    if (spec.duration <= 0.0f || t >= spec.iterations * spec.duration) {
        // Past the end (or degenerate): the pose at the exact end time.
        sample.finished = true;
        if (!spec.fillForwards) return sample;
        sample.active = true;
        const float wholeIterations = std::floor(spec.iterations);
        const float fraction = spec.iterations - wholeIterations;
        const float lastIteration =
            fraction > 0.0f ? wholeIterations : std::max(0.0f, wholeIterations - 1.0f);
        sample.progress = directed(fraction > 0.0f ? fraction : 1.0f, lastIteration);
        return sample;
    }

    const float iteration = std::floor(t / spec.duration);
    const float local = t / spec.duration - iteration;
    sample.active = true;
    sample.progress = directed(local, iteration);
    return sample;
}

/// One animatable property's keys across the keyframes, offsets sorted.
struct Track {
    Prop prop{};
    std::vector<std::pair<float, Vec4>> keys;
};

/// Resolves each keyframe's declarations against `base` (the element's
/// styled-but-unanimated ComputedStyle — em/colors resolve exactly like the
/// cascade) and builds one track per animatable property. Per CSS, missing
/// 0%/100% keyframes fall back to the base value; properties that can't
/// animate (auto/% lengths, non-animatable props) are skipped.
inline std::vector<Track> compileTracks(const std::vector<css::Keyframe>& frames,
                                        const css::ComputedStyle& base) {
    std::vector<Track> tracks;
    const auto trackFor = [&](Prop prop) -> Track* {
        for (Track& track : tracks)
            if (track.prop == prop) return &track;
        return nullptr;
    };

    for (const css::Keyframe& frame : frames) {
        css::ComputedStyle resolved = base;
        for (const Declaration& declaration : frame.declarations)
            css::applyDeclaration(declaration, &resolved);
        for (const Declaration& declaration : frame.declarations) {
            Vec4 value{};
            if (!getAnimatable(resolved, declaration.prop, &value)) continue;
            Track* track = trackFor(declaration.prop);
            if (track == nullptr) {
                tracks.push_back({declaration.prop, {}});
                track = &tracks.back();
            }
            if (!track->keys.empty() && track->keys.back().first == frame.offset)
                track->keys.back().second = value; // same offset: later wins
            else
                track->keys.push_back({frame.offset, value});
        }
    }

    // Implicit endpoints from the base style (CSS's missing from/to rule).
    for (auto it = tracks.begin(); it != tracks.end();) {
        Vec4 baseValue{};
        if (!getAnimatable(base, it->prop, &baseValue)) {
            it = tracks.erase(it); // e.g. width animated but base is auto
            continue;
        }
        if (it->keys.front().first > 0.0f)
            it->keys.insert(it->keys.begin(), {0.0f, baseValue});
        if (it->keys.back().first < 1.0f) it->keys.push_back({1.0f, baseValue});
        ++it;
    }
    return tracks;
}

/// Value at `progress` ∈ [0,1]; the timing function eases each keyframe
/// segment (CSS applies animation-timing-function between keyframes, not
/// across the whole animation).
inline Vec4 sampleTrack(const Track& track, float progress, Timing timing) {
    const auto& keys = track.keys;
    if (progress <= keys.front().first) return keys.front().second;
    if (progress >= keys.back().first) return keys.back().second;
    for (size_t i = 1; i < keys.size(); ++i) {
        if (progress > keys[i].first) continue;
        const float span = keys[i].first - keys[i - 1].first;
        const float local = span > 0.0f ? (progress - keys[i - 1].first) / span : 1.0f;
        return glm::mix(keys[i - 1].second, keys[i].second, ease(timing, local));
    }
    return keys.back().second;
}

} // namespace rendy::ui::anim
