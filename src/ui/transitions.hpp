#pragma once

// CSS transition helpers: timing-function evaluation and the mapping from
// animatable properties to interpolable values. GPU-free (unit-testable).

#include "css/computed.hpp"
#include "rendy/math/math.hpp"
#include "rendy/ui/style.hpp"

#include <cmath>
#include <vector>

namespace rendy::ui::anim {

/// y for the CSS cubic-bezier((x1,y1),(x2,y2)) curve at horizontal position
/// x ∈ [0,1] (endpoints fixed at (0,0) and (1,1)). Newton iterations on the
/// x polynomial, bisection fallback.
inline float cubicBezier(float x1, float y1, float x2, float y2, float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    const auto sample = [](float a, float b, float t) {
        // Cubic through 0, a, b, 1: ((c0*t + c1)*t + c2)*t with
        const float c2 = 3.0f * a;
        const float c1 = 3.0f * (b - a) - c2;
        const float c0 = 1.0f - c2 - c1;
        return ((c0 * t + c1) * t + c2) * t;
    };
    const auto slope = [](float a, float b, float t) {
        const float c2 = 3.0f * a;
        const float c1 = 3.0f * (b - a) - c2;
        const float c0 = 1.0f - c2 - c1;
        return (3.0f * c0 * t + 2.0f * c1) * t + c2;
    };
    float t = x;
    for (int i = 0; i < 8; ++i) {
        const float error = sample(x1, x2, t) - x;
        if (std::abs(error) < 1e-5f) return sample(y1, y2, t);
        const float d = slope(x1, x2, t);
        if (std::abs(d) < 1e-6f) break;
        t -= error / d;
        if (t <= 0.0f || t >= 1.0f) break;
    }
    // Bisection fallback for flat regions.
    float lo = 0.0f, hi = 1.0f;
    t = x;
    for (int i = 0; i < 24; ++i) {
        const float sampled = sample(x1, x2, t);
        if (std::abs(sampled - x) < 1e-5f) break;
        if (sampled < x)
            lo = t;
        else
            hi = t;
        t = 0.5f * (lo + hi);
    }
    return sample(y1, y2, t);
}

/// Progress t ∈ [0,1] through the named CSS timing function.
inline float ease(Timing timing, float t) {
    switch (timing) {
    case Timing::Linear: return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    case Timing::Ease: return cubicBezier(0.25f, 0.1f, 0.25f, 1.0f, t);
    case Timing::EaseIn: return cubicBezier(0.42f, 0.0f, 1.0f, 1.0f, t);
    case Timing::EaseOut: return cubicBezier(0.0f, 0.0f, 0.58f, 1.0f, t);
    case Timing::EaseInOut: return cubicBezier(0.42f, 0.0f, 0.58f, 1.0f, t);
    }
    return t;
}

/// Expands "all" and the border-radius alias into concrete animatable props.
inline void expandSpec(const TransitionSpec& spec, std::vector<TransitionSpec>* out) {
    const auto push = [&](Prop prop) {
        TransitionSpec expanded = spec;
        expanded.prop = prop;
        out->push_back(expanded);
    };
    if (spec.prop == Prop::Count) { // "all"
        for (Prop prop : {Prop::BackgroundColor, Prop::TextColor, Prop::BorderColor,
                          Prop::BorderWidth, Prop::Opacity, Prop::BorderRadiusTL,
                          Prop::BorderRadiusTR, Prop::BorderRadiusBR, Prop::BorderRadiusBL,
                          Prop::ShadowOffsetX, Prop::ShadowOffsetY, Prop::ShadowBlur,
                          Prop::ShadowColor, Prop::Width, Prop::Height})
            push(prop);
        return;
    }
    if (spec.prop == Prop::ShadowBlur) { // "box-shadow": every component
        for (Prop prop : {Prop::ShadowOffsetX, Prop::ShadowOffsetY, Prop::ShadowBlur,
                          Prop::ShadowColor})
            push(prop);
        return;
    }
    if (spec.prop == Prop::BorderRadiusTL) { // "border-radius": all four corners
        for (Prop prop : {Prop::BorderRadiusTL, Prop::BorderRadiusTR, Prop::BorderRadiusBR,
                          Prop::BorderRadiusBL})
            push(prop);
        return;
    }
    out->push_back(spec);
}

/// Reads the animatable value of `prop` as a Vec4 (colors: rgba; numbers:
/// x; px lengths: x). Returns false when the value can't animate (auto/%,
/// non-animatable prop).
inline bool getAnimatable(const css::ComputedStyle& s, Prop prop, Vec4* out) {
    switch (prop) {
    case Prop::BackgroundColor: *out = {s.backgroundColor.r, s.backgroundColor.g,
                                        s.backgroundColor.b, s.backgroundColor.a}; return true;
    case Prop::TextColor: *out = {s.textColor.r, s.textColor.g, s.textColor.b, s.textColor.a};
        return true;
    case Prop::BorderColor: *out = {s.borderColor.r, s.borderColor.g, s.borderColor.b,
                                    s.borderColor.a}; return true;
    case Prop::Opacity: *out = {s.opacity, 0.0f, 0.0f, 0.0f}; return true;
    case Prop::BorderWidth: *out = {s.borderWidth, 0.0f, 0.0f, 0.0f}; return true;
    case Prop::BorderRadiusTL: *out = {s.borderRadius.x, 0.0f, 0.0f, 0.0f}; return true;
    case Prop::BorderRadiusTR: *out = {s.borderRadius.y, 0.0f, 0.0f, 0.0f}; return true;
    case Prop::BorderRadiusBR: *out = {s.borderRadius.z, 0.0f, 0.0f, 0.0f}; return true;
    case Prop::BorderRadiusBL: *out = {s.borderRadius.w, 0.0f, 0.0f, 0.0f}; return true;
    case Prop::ShadowOffsetX: *out = {s.shadowOffset.x, 0.0f, 0.0f, 0.0f}; return true;
    case Prop::ShadowOffsetY: *out = {s.shadowOffset.y, 0.0f, 0.0f, 0.0f}; return true;
    case Prop::ShadowBlur: *out = {s.shadowBlur, 0.0f, 0.0f, 0.0f}; return true;
    case Prop::ShadowColor: *out = {s.shadowColor.r, s.shadowColor.g, s.shadowColor.b,
                                    s.shadowColor.a}; return true;
    case Prop::Width:
        if (s.width.unit != Unit::Px) return false;
        *out = {s.width.value, 0.0f, 0.0f, 0.0f};
        return true;
    case Prop::Height:
        if (s.height.unit != Unit::Px) return false;
        *out = {s.height.value, 0.0f, 0.0f, 0.0f};
        return true;
    default: return false;
    }
}

inline void setAnimatable(css::ComputedStyle* s, Prop prop, const Vec4& v) {
    switch (prop) {
    case Prop::BackgroundColor: s->backgroundColor = {v.x, v.y, v.z, v.w}; break;
    case Prop::TextColor: s->textColor = {v.x, v.y, v.z, v.w}; break;
    case Prop::BorderColor: s->borderColor = {v.x, v.y, v.z, v.w}; break;
    case Prop::Opacity: s->opacity = v.x; break;
    case Prop::BorderWidth: s->borderWidth = v.x; break;
    case Prop::BorderRadiusTL: s->borderRadius.x = v.x; break;
    case Prop::BorderRadiusTR: s->borderRadius.y = v.x; break;
    case Prop::BorderRadiusBR: s->borderRadius.z = v.x; break;
    case Prop::BorderRadiusBL: s->borderRadius.w = v.x; break;
    case Prop::ShadowOffsetX: s->shadowOffset.x = v.x; break;
    case Prop::ShadowOffsetY: s->shadowOffset.y = v.x; break;
    case Prop::ShadowBlur: s->shadowBlur = v.x; break;
    case Prop::ShadowColor: s->shadowColor = {v.x, v.y, v.z, v.w}; break;
    case Prop::Width: s->width = Length::px(v.x); break;
    case Prop::Height: s->height = Length::px(v.x); break;
    default: break;
    }
}

/// Whether a change to `prop` moves layout (needs Yoga + relayout).
inline bool affectsLayout(Prop prop) {
    return prop == Prop::Width || prop == Prop::Height || prop == Prop::BorderWidth;
}

} // namespace rendy::ui::anim
