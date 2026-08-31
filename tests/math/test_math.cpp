#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rendy/math/math.hpp>

using Catch::Approx;
using namespace rendy;

TEST_CASE("radians/degrees", "[math]") {
    REQUIRE(radians(180.0f) == Approx(Pi));
    REQUIRE(degrees(Pi) == Approx(180.0f));
}

TEST_CASE("Transform composes T*R*S", "[math]") {
    Transform t;
    t.position = {1.0f, 2.0f, 3.0f};
    t.scale = {2.0f, 2.0f, 2.0f};
    t.rotation = glm::angleAxis(HalfPi, Vec3{0.0f, 0.0f, 1.0f});

    // A point at +x, scaled to 2, rotated 90° around z → +y, then translated.
    const Vec4 p = t.matrix() * Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    REQUIRE(p.x == Approx(1.0f).margin(1e-5));
    REQUIRE(p.y == Approx(4.0f).margin(1e-5));
    REQUIRE(p.z == Approx(3.0f).margin(1e-5));
}

TEST_CASE("identity transform is identity matrix", "[math]") {
    const Mat4 m = Transform{}.matrix();
    REQUIRE(m == Mat4{1.0f});
}
