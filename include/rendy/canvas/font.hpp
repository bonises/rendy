#pragma once

/// \file font.hpp
/// Font handle. Id 0 is the app's default UI font (a system sans-serif found
/// at startup); load others with App::loadFont().

#include <cstdint>

namespace rendy {

struct FontRef {
    uint32_t id = 0;
};

/// Vertical metrics for a font at a given pixel size.
struct TextMetrics {
    float ascent = 0.0f;     ///< baseline to top, px
    float descent = 0.0f;    ///< baseline to bottom, px (positive)
    float lineHeight = 0.0f; ///< recommended baseline-to-baseline distance
};

} // namespace rendy
