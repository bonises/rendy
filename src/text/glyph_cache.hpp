#pragma once

// FreeType rasterization + R8 glyph atlas pages packed with stb_rect_pack.
// Glyphs are cached per (font, pixel size, glyph id) — HarfBuzz shaping
// (shapeLine) maps text to glyph ids first; pages are bindless textures the
// shared 2D pipeline samples like any other texture.

#include "gpu/texture.hpp"
#include "rendy/canvas/font.hpp"
#include "rendy/core/result.hpp"
#include "text/shape_cache.hpp"
#include "text/shaper.hpp"

#include <stb_rect_pack.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Forward-declare FreeType types to keep the header light.
struct FT_LibraryRec_;
struct FT_FaceRec_;

namespace rendy::text {

struct GlyphInfo {
    Vec2 uvMin{0.0f};
    Vec2 uvMax{0.0f};
    Vec2 size{0.0f};    ///< bitmap size, px
    Vec2 bearing{0.0f}; ///< left/top offset from pen position
    float advance = 0.0f;      ///< FreeType advance (positioning uses HarfBuzz's)
    uint32_t textureIndex = 0; ///< bindless index of the atlas page
    bool hasPixels = false;    ///< false for spaces etc.
};

class GlyphCache {
public:
    explicit GlyphCache(gpu::TexturePool& textures);
    ~GlyphCache();

    GlyphCache(const GlyphCache&) = delete;
    GlyphCache& operator=(const GlyphCache&) = delete;

    /// Loads a font file; the first successful load gets id 0 (the default).
    Result<FontRef> loadFont(const std::string& path);
    /// Tries a list of common system font paths for the default UI font.
    void loadDefaultFonts();

    [[nodiscard]] bool hasFont(uint32_t fontId) const { return fontId < faces_.size(); }

    /// Shapes one line (no '\n') into positioned glyph ids, through an LRU
    /// cache — repeated frames of the same text skip bidi + HarfBuzz. The
    /// returned vector lives in the cache; use it right away (it stays
    /// valid until the entry is evicted).
    const std::vector<ShapedGlyph>& shapeLine(uint32_t fontId, float pixelSize,
                                              std::string_view line);

    /// Rasterizes on first use, keyed by font glyph id (from shapeLine).
    /// Returns nullptr for missing fonts/glyphs.
    const GlyphInfo* glyph(uint32_t fontId, float pixelSize, uint32_t glyphIndex);

    TextMetrics metrics(uint32_t fontId, float pixelSize);

    /// Uploads any atlas pages that gained glyphs since the last call.
    void flushUploads();

private:
    struct Page {
        TextureRef texture;
        std::vector<uint8_t> pixels;
        stbrp_context pack{};
        std::vector<stbrp_node> nodes;
        bool dirty = false;
    };

    /// nullptr when the glyph can't fit any page (larger than a page).
    Page* pageWithRoom(int width, int height, int* outX, int* outY);
    void setPixelSize(uint32_t fontId, float pixelSize);

    gpu::TexturePool& textures_;
    FT_LibraryRec_* library_ = nullptr;
    std::vector<FT_FaceRec_*> faces_;
    Shaper shaper_; ///< font ids kept in lockstep with faces_
    ShapeCache shapeCache_{shaper_};
    // unique_ptr: stbrp_context self-references, so a Page must never move.
    std::vector<std::unique_ptr<Page>> pages_;
    std::unordered_map<uint64_t, GlyphInfo> glyphs_; // key: font|size|glyph id
};

} // namespace rendy::text
