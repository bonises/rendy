#pragma once

// Caret geometry on shaped text: byte offset ↔ visual x position, derived
// from the SAME shaped glyph run the renderer draws — so the caret lands
// where the glyphs actually are, including ligatures (interpolated inside
// multi-byte clusters) and RTL runs (offset 0 sits at the run's right
// edge). Pure functions — GPU-free, unit-testable.
//
// Selections use selectionRects(): a logically contiguous byte range is
// visually discontiguous across mixed-direction runs, so it yields one
// x-interval per visually contiguous piece (touching pieces merge).

#include "text/shaper.hpp"
#include "text/utf8.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace rendy::text {

/// One shaping cluster's byte range start and pen-x extent.
struct ClusterExtent {
    size_t start = 0; ///< byte offset (cluster value)
    float xMin = 0.0f;
    float xMax = 0.0f;
    bool rtl = false;
};

/// Extents of every cluster, sorted by byte offset (logical order).
inline void clusterExtents(const std::vector<ShapedGlyph>& glyphs,
                           std::vector<ClusterExtent>* out) {
    out->clear();
    float x = 0.0f;
    for (const ShapedGlyph& glyph : glyphs) {
        // Same-cluster glyphs are adjacent in shaped output.
        ClusterExtent* extent =
            !out->empty() && out->back().start == glyph.cluster ? &out->back() : nullptr;
        if (extent == nullptr) {
            out->push_back({glyph.cluster, x, x, glyph.rtl});
            extent = &out->back();
        }
        extent->xMin = std::min(extent->xMin, x);
        extent->xMax = std::max(extent->xMax, x + glyph.xAdvance);
        x += glyph.xAdvance;
    }
    std::sort(out->begin(), out->end(),
              [](const ClusterExtent& a, const ClusterExtent& b) { return a.start < b.start; });
}

/// Caret x for the caret sitting before byte `offset` (or after the last
/// character when offset == line size). Inside a multi-byte cluster (e.g.
/// an "ffi" ligature) the position interpolates linearly by bytes.
inline float caretX(const std::vector<ShapedGlyph>& glyphs, std::string_view line,
                    size_t offset) {
    if (glyphs.empty() || line.empty()) return 0.0f;
    std::vector<ClusterExtent> extents;
    clusterExtents(glyphs, &extents);
    // The cluster containing `offset` (last one starting at or before it).
    size_t index = extents.size() - 1;
    for (size_t i = 0; i < extents.size(); ++i)
        if (extents[i].start <= offset) index = i;
        else break;
    const ClusterExtent& cluster = extents[index];
    const size_t clusterEnd =
        index + 1 < extents.size() ? extents[index + 1].start : line.size();
    const size_t clusterBytes = std::max<size_t>(clusterEnd - cluster.start, 1);
    const float fraction =
        std::clamp(static_cast<float>(std::min(offset, clusterEnd) - cluster.start) /
                       static_cast<float>(clusterBytes),
                   0.0f, 1.0f);
    const float width = cluster.xMax - cluster.xMin;
    return cluster.rtl ? cluster.xMax - fraction * width : cluster.xMin + fraction * width;
}

/// Byte offset (a codepoint boundary) whose caret is closest to `x`.
inline size_t caretFromX(const std::vector<ShapedGlyph>& glyphs, std::string_view line,
                         float x) {
    size_t best = 0;
    float bestDistance = std::abs(x - caretX(glyphs, line, 0));
    size_t offset = 0;
    while (offset < line.size()) {
        decodeUtf8(line, offset);
        const float distance = std::abs(x - caretX(glyphs, line, offset));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = offset;
        }
    }
    return best;
}

/// X-intervals (relative to the text origin) covering the selection
/// [begin, end) in bytes. Each cluster's selected byte sub-range maps to
/// an x sub-range exactly like caretX (linear inside clusters, mirrored
/// in RTL); visually touching or overlapping pieces merge, so
/// single-direction selections still come back as one interval while
/// mixed-direction ones yield one interval per contiguous piece.
inline void selectionRects(const std::vector<ShapedGlyph>& glyphs, std::string_view line,
                           size_t begin, size_t end,
                           std::vector<std::pair<float, float>>* out) {
    out->clear();
    end = std::min(end, line.size());
    if (begin >= end || glyphs.empty() || line.empty()) return;
    std::vector<ClusterExtent> extents;
    clusterExtents(glyphs, &extents);
    for (size_t i = 0; i < extents.size(); ++i) {
        const ClusterExtent& cluster = extents[i];
        const size_t clusterEnd =
            i + 1 < extents.size() ? extents[i + 1].start : line.size();
        const size_t lo = std::max(begin, cluster.start);
        const size_t hi = std::min(end, clusterEnd);
        if (lo >= hi) continue;
        const size_t clusterBytes = std::max<size_t>(clusterEnd - cluster.start, 1);
        const float f0 =
            static_cast<float>(lo - cluster.start) / static_cast<float>(clusterBytes);
        const float f1 =
            static_cast<float>(hi - cluster.start) / static_cast<float>(clusterBytes);
        const float width = cluster.xMax - cluster.xMin;
        if (cluster.rtl)
            out->push_back({cluster.xMax - f1 * width, cluster.xMax - f0 * width});
        else
            out->push_back({cluster.xMin + f0 * width, cluster.xMin + f1 * width});
    }
    if (out->empty()) return;
    std::sort(out->begin(), out->end());
    // Merge pieces that touch (half-pixel slack absorbs float noise).
    size_t merged = 0;
    for (size_t i = 1; i < out->size(); ++i) {
        if ((*out)[i].first <= (*out)[merged].second + 0.5f)
            (*out)[merged].second = std::max((*out)[merged].second, (*out)[i].second);
        else
            (*out)[++merged] = (*out)[i];
    }
    out->resize(merged + 1);
}

/// Sorted valid break offsets for emergency line breaking: every cluster
/// start plus the end of the text. Breaking anywhere else would split a
/// shaping cluster (combining marks, ligated sequences) in half.
inline void clusterBreaks(const std::vector<ShapedGlyph>& glyphs, size_t textSize,
                          std::vector<size_t>* out) {
    out->clear();
    for (const ShapedGlyph& glyph : glyphs) out->push_back(glyph.cluster);
    out->push_back(textSize);
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
}

} // namespace rendy::text
