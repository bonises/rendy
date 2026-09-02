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
