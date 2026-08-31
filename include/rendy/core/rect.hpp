#pragma once

/// \file rect.hpp
/// Axis-aligned rectangle in pixels: top-left origin, +y down (screen space).

#include "../math/math.hpp"

#include <algorithm>

namespace rendy {

struct Rect {
    Vec2 pos{0.0f};  ///< top-left corner
    Vec2 size{0.0f}; ///< width, height (non-negative)

    [[nodiscard]] float left() const { return pos.x; }
    [[nodiscard]] float top() const { return pos.y; }
    [[nodiscard]] float right() const { return pos.x + size.x; }
    [[nodiscard]] float bottom() const { return pos.y + size.y; }
    [[nodiscard]] Vec2 center() const { return pos + size * 0.5f; }
    [[nodiscard]] bool empty() const { return size.x <= 0.0f || size.y <= 0.0f; }

    [[nodiscard]] bool contains(Vec2 p) const {
        return p.x >= left() && p.x < right() && p.y >= top() && p.y < bottom();
    }

    /// Intersection; empty rect (size 0) when disjoint.
    [[nodiscard]] Rect intersect(const Rect& other) const {
        const float x0 = std::max(left(), other.left());
        const float y0 = std::max(top(), other.top());
        const float x1 = std::min(right(), other.right());
        const float y1 = std::min(bottom(), other.bottom());
        if (x1 <= x0 || y1 <= y0) return Rect{{x0, y0}, {0.0f, 0.0f}};
        return Rect{{x0, y0}, {x1 - x0, y1 - y0}};
    }

    [[nodiscard]] bool overlaps(const Rect& other) const {
        return left() < other.right() && other.left() < right() &&
               top() < other.bottom() && other.top() < bottom();
    }

    [[nodiscard]] Rect expanded(float amount) const {
        return Rect{pos - Vec2{amount}, size + Vec2{2.0f * amount}};
    }

    friend bool operator==(const Rect&, const Rect&) = default;
};

} // namespace rendy
