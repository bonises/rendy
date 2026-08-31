#include <catch2/catch_test_macros.hpp>

#include <rendy/core/log.hpp>
#include <rendy/core/result.hpp>

#include <memory>
#include <string>

using rendy::Error;
using rendy::Result;

TEST_CASE("Result holds a value", "[core][result]") {
    Result<int> r = 42;
    REQUIRE(r.hasValue());
    REQUIRE(static_cast<bool>(r));
    REQUIRE(r.value() == 42);
    REQUIRE(r.valueOr(7) == 42);
}

TEST_CASE("Result holds an error", "[core][result]") {
    Result<int> r = rendy::err("boom");
    REQUIRE_FALSE(r.hasValue());
    REQUIRE(r.error().message == "boom");
    REQUIRE(r.valueOr(7) == 7);
}

TEST_CASE("err formats with fmt", "[core][result]") {
    Error e = rendy::err("bad size {}x{}", 3, 4);
    REQUIRE(e.message == "bad size 3x4");
}

TEST_CASE("Result moves move-only types", "[core][result]") {
    Result<std::unique_ptr<int>> r = std::make_unique<int>(5);
    REQUIRE(r.hasValue());
    std::unique_ptr<int> owned = std::move(r).value();
    REQUIRE(*owned == 5);
}

TEST_CASE("Result<void> defaults to success", "[core][result]") {
    Result<void> ok;
    REQUIRE(ok.hasValue());
    Result<void> bad = rendy::err("nope");
    REQUIRE_FALSE(bad.hasValue());
    REQUIRE(bad.error().message == "nope");
}
