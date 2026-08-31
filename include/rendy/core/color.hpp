#pragma once

/// \file color.hpp
/// Colors are specified in sRGB space with straight (non-premultiplied)
/// alpha, as floats in [0,1]. Shaders convert to linear where needed.

#include <cstdint>

namespace rendy {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    /// From 0xRRGGBBAA, e.g. Color::rgba(0xE74C3CFF).
    static constexpr Color rgba(uint32_t hex) {
        return Color{
            static_cast<float>((hex >> 24) & 0xFF) / 255.0f,
            static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
            static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
            static_cast<float>(hex & 0xFF) / 255.0f,
        };
    }

    /// From 0xRRGGBB with alpha = 1, e.g. Color::rgb(0xE74C3C).
    static constexpr Color rgb(uint32_t hex) {
        return Color{
            static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
            static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
            static_cast<float>(hex & 0xFF) / 255.0f,
            1.0f,
        };
    }

    /// Same color with alpha multiplied by `factor`.
    [[nodiscard]] constexpr Color fade(float factor) const {
        return Color{r, g, b, a * factor};
    }

    /// Packed 0xRRGGBBAA (clamps to [0,1]).
    [[nodiscard]] constexpr uint32_t packed() const {
        auto to8 = [](float v) -> uint32_t {
            if (v <= 0.0f) return 0;
            if (v >= 1.0f) return 255;
            return static_cast<uint32_t>(v * 255.0f + 0.5f);
        };
        return (to8(r) << 24) | (to8(g) << 16) | (to8(b) << 8) | to8(a);
    }

    friend constexpr bool operator==(const Color&, const Color&) = default;
};

/// Import with `using namespace rendy::literals;` (implied by
/// `using namespace rendy;`).
inline namespace literals {
/// `0xE74C3CFF_rgba` → Color
constexpr Color operator""_rgba(unsigned long long hex) {
    return Color::rgba(static_cast<uint32_t>(hex));
}
/// `0xE74C3C_rgb` → Color (alpha 1)
constexpr Color operator""_rgb(unsigned long long hex) {
    return Color::rgb(static_cast<uint32_t>(hex));
}
} // namespace literals

namespace colors {
inline constexpr Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
inline constexpr Color black = Color::rgb(0x000000);
inline constexpr Color white = Color::rgb(0xFFFFFF);
inline constexpr Color red = Color::rgb(0xE74C3C);
inline constexpr Color green = Color::rgb(0x2ECC71);
inline constexpr Color blue = Color::rgb(0x3498DB);
inline constexpr Color yellow = Color::rgb(0xF1C40F);
inline constexpr Color orange = Color::rgb(0xE67E22);
inline constexpr Color purple = Color::rgb(0x9B59B6);
inline constexpr Color gray = Color::rgb(0x808080);
inline constexpr Color slate = Color::rgb(0x1E1E2E);
} // namespace colors

} // namespace rendy
