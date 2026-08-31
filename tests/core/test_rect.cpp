#include <catch2/catch_test_macros.hpp>

#include <rendy/core/rect.hpp>

using rendy::Rect;
using rendy::Vec2;

TEST_CASE("Rect basics", "[core][rect]") {
    const Rect r{{10.0f, 20.0f}, {100.0f, 50.0f}};
    REQUIRE(r.right() == 110.0f);
    REQUIRE(r.bottom() == 70.0f);
    REQUIRE(r.center() == Vec2{60.0f, 45.0f});
    REQUIRE_FALSE(r.empty());
    REQUIRE(Rect{}.empty());
}

TEST_CASE("Rect contains is half-open", "[core][rect]") {
    const Rect r{{0.0f, 0.0f}, {10.0f, 10.0f}};
    REQUIRE(r.contains({0.0f, 0.0f}));
    REQUIRE(r.contains({9.9f, 9.9f}));
    REQUIRE_FALSE(r.contains({10.0f, 5.0f}));
    REQUIRE_FALSE(r.contains({-0.1f, 5.0f}));
}

TEST_CASE("Rect intersect", "[core][rect]") {
    const Rect a{{0.0f, 0.0f}, {10.0f, 10.0f}};
    const Rect b{{5.0f, 5.0f}, {10.0f, 10.0f}};
    REQUIRE(a.intersect(b) == Rect{{5.0f, 5.0f}, {5.0f, 5.0f}});
    REQUIRE(a.overlaps(b));

    const Rect c{{20.0f, 20.0f}, {5.0f, 5.0f}};
    REQUIRE(a.intersect(c).empty());
    REQUIRE_FALSE(a.overlaps(c));
}

TEST_CASE("Rect expanded", "[core][rect]") {
    const Rect r = Rect{{10.0f, 10.0f}, {10.0f, 10.0f}}.expanded(2.0f);
    REQUIRE(r == Rect{{8.0f, 8.0f}, {14.0f, 14.0f}});
}
