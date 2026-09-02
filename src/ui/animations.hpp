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
/// Each key can carry its own easing (CSS `animation-timing-function`
/// inside the keyframe block), applying to the segment that *starts* there.
struct Track {
    struct Key {
        float offset = 0.0f;
        Vec4 value{};
        bool hasTiming = false;
        Timing timing = Timing::Ease;
    };
    Prop prop{};
    std::vector<Key> keys;
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
            if (!track->keys.empty() && track->keys.back().offset == frame.offset) {
                track->keys.back().value = value; // same offset: later wins
                if (frame.hasTiming) {
                    track->keys.back().hasTiming = true;
                    track->keys.back().timing = frame.timing;
                }
            } else {
                track->keys.push_back({frame.offset, value, frame.hasTiming, frame.timing});
            }
        }
    }

    // Implicit endpoints from the base style (CSS's missing from/to rule);
    // an implicit `from` eases with the animation's own timing function.
    for (auto it = tracks.begin(); it != tracks.end();) {
        Vec4 baseValue{};
        if (!getAnimatable(base, it->prop, &baseValue)) {
            it = tracks.erase(it); // e.g. width animated but base is auto
            continue;
        }
        if (it->keys.front().offset > 0.0f)
            it->keys.insert(it->keys.begin(), {0.0f, baseValue, false, Timing::Ease});
        if (it->keys.back().offset < 1.0f)
            it->keys.push_back({1.0f, baseValue, false, Timing::Ease});
        ++it;
    }
    return tracks;
}

/// Value at `progress` ∈ [0,1]; each keyframe segment eases with the
/// timing function of the keyframe it starts at (CSS's per-keyframe
/// animation-timing-function), falling back to the animation's own timing.
inline Vec4 sampleTrack(const Track& track, float progress, Timing fallback) {
    const auto& keys = track.keys;
    if (progress <= keys.front().offset) return keys.front().value;
    if (progress >= keys.back().offset) return keys.back().value;
    for (size_t i = 1; i < keys.size(); ++i) {
        if (progress > keys[i].offset) continue;
        const float span = keys[i].offset - keys[i - 1].offset;
        const float local = span > 0.0f ? (progress - keys[i - 1].offset) / span : 1.0f;
        const Timing timing = keys[i - 1].hasTiming ? keys[i - 1].timing : fallback;
        return glm::mix(keys[i - 1].value, keys[i].value, ease(timing, local));
    }
    return keys.back().value;
}

} // namespace rendy::ui::anim
