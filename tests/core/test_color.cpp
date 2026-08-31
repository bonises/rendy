#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rendy/core/color.hpp>

using Catch::Approx;
using rendy::Color;
using namespace rendy::literals;

TEST_CASE("Color from hex", "[core][color]") {
    const Color c = Color::rgba(0xFF800040);
    REQUIRE(c.r == Approx(1.0f));
    REQUIRE(c.g == Approx(128.0f / 255.0f));
    REQUIRE(c.b == Approx(0.0f));
    REQUIRE(c.a == Approx(64.0f / 255.0f));

    REQUIRE(Color::rgb(0x112233) == Color::rgba(0x112233FF));
}

TEST_CASE("Color literals", "[core][color]") {
    REQUIRE(0xE74C3CFF_rgba == Color::rgb(0xE74C3C));
    REQUIRE(0xE74C3C_rgb == Color::rgb(0xE74C3C));
}

TEST_CASE("Color round-trips through packed", "[core][color]") {
    const Color c = Color::rgba(0x12345678);
    REQUIRE(c.packed() == 0x12345678u);
    // Out-of-range components clamp.
    REQUIRE(Color{2.0f, -1.0f, 0.5f, 1.0f}.packed() == 0xFF0080FFu);
}

TEST_CASE("fade multiplies alpha", "[core][color]") {
    const Color c = rendy::colors::white.fade(0.5f);
    REQUIRE(c.a == Approx(0.5f));
    REQUIRE(c.r == Approx(1.0f));
}
