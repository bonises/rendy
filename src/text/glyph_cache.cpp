#include "text/glyph_cache.hpp"

#include "rendy/core/log.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace rendy::text {
namespace {

constexpr int kPageSize = 1024;
constexpr int kGlyphPadding = 1;

uint64_t glyphKey(uint32_t fontId, uint32_t pixelSize, uint32_t glyphIndex) {
    // 8 bits font, 12 bits size, 32 bits glyph id.
    return (static_cast<uint64_t>(fontId) << 56) | (static_cast<uint64_t>(pixelSize) << 44) |
           glyphIndex;
}

const char* const kDefaultFontPaths[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
};

} // namespace

GlyphCache::GlyphCache(gpu::TexturePool& textures) : textures_(textures) {
    if (FT_Init_FreeType(&library_) != 0) {
        log::error("FT_Init_FreeType failed — text rendering disabled");
        library_ = nullptr;
    }
}

GlyphCache::~GlyphCache() {
    for (FT_Face face : faces_) FT_Done_Face(face);
    if (library_ != nullptr) FT_Done_FreeType(library_);
}

Result<FontRef> GlyphCache::loadFont(const std::string& path) {
    if (library_ == nullptr) return err("FreeType unavailable");
    FT_Face face = nullptr;
    if (FT_New_Face(library_, path.c_str(), 0, &face) != 0)
        return err("failed to load font '{}'", path);
    // The shaper claims a slot even on failure, keeping ids in lockstep;
    // it reads the same file FreeType just parsed, so failure is unlikely.
    if (auto shaped = shaper_.loadFont(path); !shaped)
        log::warn("{} — text in this font won't shape", shaped.error().message);
    faces_.push_back(face);
    return FontRef{static_cast<uint32_t>(faces_.size() - 1)};
}

void GlyphCache::loadDefaultFonts() {
    for (const char* path : kDefaultFontPaths) {
        if (!std::filesystem::exists(path)) continue;
        if (loadFont(path)) {
            log::debug("default font: {}", path);
            return;
        }
    }
    log::warn("no default font found — drawText needs an explicitly loaded font");
}

void GlyphCache::setPixelSize(uint32_t fontId, float pixelSize) {
    // Clamp: absurd sizes (e.g. a runaway CSS font-size) would overflow the
    // atlas and waste memory.
    FT_Set_Pixel_Sizes(
        faces_[fontId], 0,
        static_cast<FT_UInt>(std::lround(std::clamp(pixelSize, 1.0f, 512.0f))));
}

TextMetrics GlyphCache::metrics(uint32_t fontId, float pixelSize) {
    if (!hasFont(fontId)) return {};
    setPixelSize(fontId, pixelSize);
    const FT_Size_Metrics& m = faces_[fontId]->size->metrics;
    return TextMetrics{
        static_cast<float>(m.ascender) / 64.0f,
        static_cast<float>(-m.descender) / 64.0f,
        static_cast<float>(m.height) / 64.0f,
    };
}

const std::vector<ShapedGlyph>& GlyphCache::shapeLine(uint32_t fontId, float pixelSize,
                                                      std::string_view line) {
    shaper_.shape(fontId, pixelSize, line, &shapeScratch_);
    return shapeScratch_;
}

GlyphCache::Page* GlyphCache::pageWithRoom(int width, int height, int* outX, int* outY) {
    // A glyph larger than a page can never pack; stbrp then reports failure
    // with garbage coordinates, so reject it up front.
    if (width + kGlyphPadding > kPageSize || height + kGlyphPadding > kPageSize) {
        log::warn("glyph too large for atlas ({}x{} px), skipping", width, height);
        return nullptr;
    }
    for (auto& page : pages_) {
        stbrp_rect rect{};
        rect.w = static_cast<stbrp_coord>(width + kGlyphPadding);
        rect.h = static_cast<stbrp_coord>(height + kGlyphPadding);
        if (stbrp_pack_rects(&page->pack, &rect, 1) != 0) {
            *outX = rect.x;
            *outY = rect.y;
            return page.get();
        }
    }
    // New page. Initialize the packer in place — never before a move.
    pages_.push_back(std::make_unique<Page>());
    Page& newPage = *pages_.back();
    newPage.pixels.assign(static_cast<size_t>(kPageSize) * kPageSize, 0);
    newPage.nodes.resize(kPageSize);
    stbrp_init_target(&newPage.pack, kPageSize, kPageSize, newPage.nodes.data(), kPageSize);
    auto texture = textures_.createR8(newPage.pixels.data(), {kPageSize, kPageSize});
    if (texture)
        newPage.texture = texture.value();
    else
        log::error("glyph atlas page creation failed: {}", texture.error().message);
    stbrp_rect rect{};
    rect.w = static_cast<stbrp_coord>(width + kGlyphPadding);
    rect.h = static_cast<stbrp_coord>(height + kGlyphPadding);
    if (stbrp_pack_rects(&newPage.pack, &rect, 1) == 0) return nullptr; // unreachable
    *outX = rect.x;
    *outY = rect.y;
    return &newPage;
}

const GlyphInfo* GlyphCache::glyph(uint32_t fontId, float pixelSize, uint32_t glyphIndex) {
    if (!hasFont(fontId)) return nullptr;
    const auto sizeKey =
        static_cast<uint32_t>(std::lround(std::clamp(pixelSize, 1.0f, 512.0f)));
    const uint64_t key = glyphKey(fontId, sizeKey, glyphIndex);
    if (auto it = glyphs_.find(key); it != glyphs_.end()) return &it->second;

    FT_Face face = faces_[fontId];
    setPixelSize(fontId, pixelSize);
    if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0)
        return nullptr;

    const FT_GlyphSlot slot = face->glyph;
    const FT_Bitmap& bitmap = slot->bitmap;

    GlyphInfo info;
    info.advance = static_cast<float>(slot->advance.x) / 64.0f;
    info.size = {static_cast<float>(bitmap.width), static_cast<float>(bitmap.rows)};
    info.bearing = {static_cast<float>(slot->bitmap_left), static_cast<float>(slot->bitmap_top)};
    info.hasPixels = bitmap.width > 0 && bitmap.rows > 0;

    if (info.hasPixels) {
        int x = 0;
        int y = 0;
        Page* page = pageWithRoom(static_cast<int>(bitmap.width),
                                  static_cast<int>(bitmap.rows), &x, &y);
        if (page == nullptr) {
            info.hasPixels = false; // renders as blank; advance still applies
        } else {
            for (unsigned row = 0; row < bitmap.rows; ++row) {
                const uint8_t* src = bitmap.buffer + row * static_cast<unsigned>(bitmap.pitch);
                uint8_t* dst =
                    page->pixels.data() + (static_cast<size_t>(y) + row) * kPageSize + x;
                std::memcpy(dst, src, bitmap.width);
            }
            page->dirty = true;
            const float scale = 1.0f / kPageSize;
            info.uvMin = {static_cast<float>(x) * scale, static_cast<float>(y) * scale};
            info.uvMax = {(static_cast<float>(x) + info.size.x) * scale,
                          (static_cast<float>(y) + info.size.y) * scale};
            info.textureIndex = page->texture.index;
        }
    }

    return &glyphs_.emplace(key, info).first->second;
}

void GlyphCache::flushUploads() {
    for (auto& page : pages_) {
        if (!page->dirty) continue;
        textures_.update(page->texture, page->pixels.data(), page->pixels.size());
        page->dirty = false;
    }
}

} // namespace rendy::text
