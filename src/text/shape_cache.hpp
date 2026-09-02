#pragma once

// LRU cache in front of the Shaper. Editors invalidate whole viewports on
// every keystroke and caret blink, and re-shaping (bidi + HarfBuzz) every
// visible line each frame would dominate CPU time — with the cache only
// the changed line re-shapes. Keyed by (font, quantized pixel size, text);
// the Shaper is deterministic and font slots are never reused, so entries
// never go stale. Pure CPU, unit-testable. Lookups on the hit path are
// allocation-free (heterogeneous string_view lookup).

#include "text/shaper.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rendy::text {

class ShapeCache {
public:
    /// `capacity` is an entry count; at ~16 bytes per glyph a full cache
    /// of 200-character lines stays in the low megabytes.
    explicit ShapeCache(Shaper& shaper, size_t capacity = 2048)
        : shaper_(shaper), capacity_(std::max<size_t>(capacity, 1)) {}

    /// Shaped glyphs for one line (no '\n'), cached. The reference stays
    /// valid until the entry is evicted — use it right away.
    const std::vector<ShapedGlyph>& shape(uint32_t fontId, float pixelSize,
                                          std::string_view text) {
        // Quantize exactly like the Shaper does, so requests that shape
        // identically share an entry.
        const auto size =
            static_cast<int32_t>(std::lround(std::clamp(pixelSize, 1.0f, 512.0f) * 64.0f));
        if (const auto it = map_.find(KeyView{fontId, size, text}); it != map_.end()) {
            ++hits_;
            entries_.splice(entries_.begin(), entries_, it->second);
            return entries_.front().glyphs;
        }
        ++misses_;
        entries_.push_front({{fontId, size, std::string(text)}, {}});
        shaper_.shape(fontId, pixelSize, text, &entries_.front().glyphs);
        map_.emplace(entries_.front().key, entries_.begin());
        if (entries_.size() > capacity_) {
            // (find + erase-by-iterator: heterogeneous erase is C++23)
            map_.erase(map_.find(KeyView{entries_.back().key.font, entries_.back().key.size,
                                         entries_.back().key.text}));
            entries_.pop_back();
        }
        return entries_.front().glyphs;
    }

    [[nodiscard]] size_t hits() const { return hits_; }
    [[nodiscard]] size_t misses() const { return misses_; }
    [[nodiscard]] size_t size() const { return entries_.size(); }

private:
    struct Key {
        uint32_t font = 0;
        int32_t size = 0; ///< 26.6 fixed point, like the Shaper's scale
        std::string text;
    };
    struct KeyView {
        uint32_t font = 0;
        int32_t size = 0;
        std::string_view text;
    };
    struct Hash {
        using is_transparent = void;
        static size_t mix(uint32_t font, int32_t size, std::string_view text) {
            size_t h = std::hash<std::string_view>{}(text);
            h ^= (static_cast<size_t>(font) + 0x9E3779B97F4A7C15ull) + (h << 6) + (h >> 2);
            h ^= (static_cast<size_t>(static_cast<uint32_t>(size)) + 0x9E3779B97F4A7C15ull) +
                 (h << 6) + (h >> 2);
            return h;
        }
        size_t operator()(const Key& k) const { return mix(k.font, k.size, k.text); }
        size_t operator()(const KeyView& k) const { return mix(k.font, k.size, k.text); }
    };
    struct Equal {
        using is_transparent = void;
        template <typename A, typename B>
        bool operator()(const A& a, const B& b) const {
            return a.font == b.font && a.size == b.size &&
                   std::string_view(a.text) == std::string_view(b.text);
        }
    };
    struct Entry {
        Key key;
        std::vector<ShapedGlyph> glyphs;
    };

    Shaper& shaper_;
    size_t capacity_;
    std::list<Entry> entries_; ///< front = most recently used
    std::unordered_map<Key, std::list<Entry>::iterator, Hash, Equal> map_;
    size_t hits_ = 0;
    size_t misses_ = 0;
};

} // namespace rendy::text
