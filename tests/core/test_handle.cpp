#include <catch2/catch_test_macros.hpp>

#include <rendy/core/handle.hpp>

#include <string>

namespace {
struct TestTag {};
using Pool = rendy::HandlePool<std::string, TestTag>;
} // namespace

TEST_CASE("HandlePool create/get/destroy", "[core][handle]") {
    Pool pool;
    auto a = pool.create("alpha");
    auto b = pool.create("beta");

    REQUIRE(pool.aliveCount() == 2);
    REQUIRE(*pool.get(a) == "alpha");
    REQUIRE(*pool.get(b) == "beta");

    REQUIRE(pool.destroy(a));
    REQUIRE(pool.get(a) == nullptr);
    REQUIRE(pool.aliveCount() == 1);
    REQUIRE_FALSE(pool.destroy(a)); // double destroy is a no-op
}

TEST_CASE("stale handles do not alias recycled slots", "[core][handle]") {
    Pool pool;
    auto a = pool.create("first");
    pool.destroy(a);
    auto b = pool.create("second"); // reuses slot 0

    REQUIRE(b.index == a.index);
    REQUIRE(b.generation != a.generation);
    REQUIRE(pool.get(a) == nullptr);
    REQUIRE(*pool.get(b) == "second");
}

TEST_CASE("default handle is invalid", "[core][handle]") {
    rendy::Handle<TestTag> h;
    REQUIRE_FALSE(h.valid());
    Pool pool;
    REQUIRE(pool.get(h) == nullptr);
}

TEST_CASE("forEach visits only live slots", "[core][handle]") {
    Pool pool;
    auto a = pool.create("a");
    pool.create("b");
    pool.destroy(a);

    int count = 0;
    pool.forEach([&](Pool::HandleType, std::string& v) {
        REQUIRE(v == "b");
        count++;
    });
    REQUIRE(count == 1);
}
