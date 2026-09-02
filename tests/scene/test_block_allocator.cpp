#include <catch2/catch_test_macros.hpp>

#include "scene/block_allocator.hpp"

#include <cstdint>

using rendy::detail::BlockAllocator;

TEST_CASE("bump allocation grows end", "[scene][alloc]") {
    BlockAllocator alloc;
    REQUIRE(alloc.allocate(100) == 0);
    REQUIRE(alloc.allocate(50) == 100);
    REQUIRE(alloc.end() == 150);
    REQUIRE(alloc.freeUnits() == 0);
}

TEST_CASE("freed blocks are reused first-fit", "[scene][alloc]") {
    BlockAllocator alloc;
    const uint32_t a = alloc.allocate(100);
    const uint32_t b = alloc.allocate(100);
    const uint32_t c = alloc.allocate(100);
    (void)c;
    alloc.free(b, 100);
    REQUIRE(alloc.freeUnits() == 100);
    // Exact fit reuses the hole entirely.
    REQUIRE(alloc.allocate(100) == b);
    REQUIRE(alloc.freeUnits() == 0);
    // Smaller alloc splits the hole.
    alloc.free(a, 100);
    REQUIRE(alloc.allocate(30) == a);
    REQUIRE(alloc.freeUnits() == 70);
    REQUIRE(alloc.allocate(70) == a + 30);
    REQUIRE(alloc.end() == 300);
}

TEST_CASE("adjacent frees coalesce", "[scene][alloc]") {
    BlockAllocator alloc;
    const uint32_t a = alloc.allocate(10);
    const uint32_t b = alloc.allocate(10);
    const uint32_t c = alloc.allocate(10);
    (void)alloc.allocate(10); // keeps the tail busy so end() stays put
    alloc.free(a, 10);
    alloc.free(c, 10);
    REQUIRE(alloc.freeUnits() == 20);
    alloc.free(b, 10); // bridges a..c into one 30-unit block
    REQUIRE(alloc.freeUnits() == 30);
    REQUIRE(alloc.allocate(30) == a); // the whole span fits one alloc
}

TEST_CASE("tail frees shrink end", "[scene][alloc]") {
    BlockAllocator alloc;
    const uint32_t a = alloc.allocate(64);
    const uint32_t b = alloc.allocate(64);
    REQUIRE(alloc.end() == 128);
    alloc.free(b, 64);
    REQUIRE(alloc.end() == 64); // returned to the bump region
    REQUIRE(alloc.freeUnits() == 0);
    alloc.free(a, 64);
    REQUIRE(alloc.end() == 0);
}

TEST_CASE("too-small holes are skipped", "[scene][alloc]") {
    BlockAllocator alloc;
    const uint32_t a = alloc.allocate(10);
    (void)alloc.allocate(100);
    alloc.free(a, 10);
    // 20 doesn't fit the 10-unit hole: bump-allocated at the end.
    REQUIRE(alloc.allocate(20) == 110);
    // 10 fits it exactly.
    REQUIRE(alloc.allocate(10) == a);
}
