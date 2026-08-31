#pragma once

/// \file texture.hpp
/// Public texture handle. Create via App::loadTexture / App::createTexture;
/// draw via Canvas::drawImage or 3D materials. The index is the texture's
/// slot in the global bindless table — stable for the texture's lifetime.

#include "../math/math.hpp"

#include <cstdint>

namespace rendy {

struct TextureRef {
    uint32_t index = 0; ///< 0 = invalid (the built-in white texture)
    IVec2 size{0, 0};

    [[nodiscard]] bool valid() const { return index != 0; }
    explicit operator bool() const { return valid(); }
};

struct TextureOptions {
    enum class Filter { Linear, Nearest };
    enum class Wrap { Clamp, Repeat };
    Filter filter = Filter::Linear;
    Wrap wrap = Wrap::Clamp;
    /// Treat pixel data as sRGB (UI images, albedo). Off for data textures.
    bool srgb = true;
};

} // namespace rendy
