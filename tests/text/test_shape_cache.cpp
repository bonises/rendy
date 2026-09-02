#include <catch2/catch_test_macros.hpp>

#include "text/shape_cache.hpp"

#include <filesystem>

using namespace rendy;
using namespace rendy::text;

namespace {

const char* dejaVu() {
    for (const char* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                             "/usr/share/fonts/TTF/DejaVuSans.ttf"})
        if (std::filesystem::exists(path)) return path;
    return nullptr;
}

} // namespace

TEST_CASE("shape cache: hits skip shaping and return identical glyphs", "[text][shaper]") {
    const char* fontPath = dejaVu();
    if (fontPath == nullptr) {
        SUCCEED("no system DejaVu font — skipping");
        return;
    }
    Shaper shaper;
    const uint32_t id = shaper.loadFont(fontPath).value();
    ShapeCache cache(shaper);

    const auto& first = cache.shape(id, 16.0f, "hej سلام");
    REQUIRE(cache.misses() == 1);
    std::vector<ShapedGlyph> reference;
    REQUIRE(shaper.shape(id, 16.0f, "hej سلام", &reference));
    REQUIRE(first.size() == reference.size());
    for (size_t i = 0; i < first.size(); ++i) {
        REQUIRE(first[i].glyphIndex == reference[i].glyphIndex);
        REQUIRE(first[i].cluster == reference[i].cluster);
        REQUIRE(first[i].xAdvance == reference[i].xAdvance);
    }

    // Same text again: a hit, same entry (stable address, no re-shape).
    const auto& second = cache.shape(id, 16.0f, "hej سلام");
    REQUIRE(cache.hits() == 1);
    REQUIRE(&second == &first);

    // Sub-quantum size difference maps to the same 26.6 scale → still a hit.
    (void)cache.shape(id, 16.001f, "hej سلام");
    REQUIRE(cache.hits() == 2);

    // Different text or size: misses.
    (void)cache.shape(id, 16.0f, "hej salam");
    (void)cache.shape(id, 17.0f, "hej سلام");
    REQUIRE(cache.misses() == 3);
}

TEST_CASE("shape cache: least recently used entries evict", "[text][shaper]") {
    const char* fontPath = dejaVu();
    if (fontPath == nullptr) {
        SUCCEED("no system DejaVu font — skipping");
        return;
    }
    Shaper shaper;
    const uint32_t id = shaper.loadFont(fontPath).value();
    ShapeCache cache(shaper, 2);

    (void)cache.shape(id, 16.0f, "a");
    (void)cache.shape(id, 16.0f, "b");
    (void)cache.shape(id, 16.0f, "a"); // touch: a is now most recent
    REQUIRE(cache.hits() == 1);
    (void)cache.shape(id, 16.0f, "c"); // evicts b (the LRU), not a
    REQUIRE(cache.size() == 2);

    (void)cache.shape(id, 16.0f, "a");
    REQUIRE(cache.hits() == 2); // survived
    (void)cache.shape(id, 16.0f, "b");
    REQUIRE(cache.misses() == 4); // evicted: re-shaped
}

TEST_CASE("shape cache: byte budget evicts oversized content", "[text][shaper]") {
    const char* fontPath = dejaVu();
    if (fontPath == nullptr) {
        SUCCEED("no system DejaVu font — skipping");
        return;
    }
    Shaper shaper;
    const uint32_t id = shaper.loadFont(fontPath).value();
    // Generous entry cap, tiny byte budget: bytes drive eviction.
    ShapeCache cache(shaper, 100, 4096);

    const std::string big(600, 'x'); // ≈ 600*2 key + 600*16 glyph bytes
    (void)cache.shape(id, 16.0f, big + "1");
    (void)cache.shape(id, 16.0f, big + "2");
    REQUIRE(cache.size() == 1); // the first entry had to go
    REQUIRE(cache.bytes() <= 4096 + 12000); // ≈ one entry's worth

    // A single entry over the whole budget still caches (and returns) —
    // the live reference is never evicted underneath the caller.
    const std::string huge(5000, 'y');
    const auto& glyphs = cache.shape(id, 16.0f, huge);
    REQUIRE(glyphs.size() == 5000);
    REQUIRE(cache.size() == 1);

    // Small lines are unaffected by the budget.
    ShapeCache roomy(shaper, 100, 1 << 20);
    for (int i = 0; i < 50; ++i)
        (void)roomy.shape(id, 16.0f, "line " + std::to_string(i));
    REQUIRE(roomy.size() == 50);
}
