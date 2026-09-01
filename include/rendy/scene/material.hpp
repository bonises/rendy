#pragma once

/// \file material.hpp
/// PBR metallic-roughness materials, matching glTF 2.0 semantics.

#include "../core/color.hpp"
#include "../gpu/texture.hpp"

namespace rendy {

/// How a material's alpha is interpreted (glTF semantics).
enum class AlphaMode : uint8_t {
    Opaque, ///< alpha ignored
    Mask,   ///< discard below alphaCutoff (foliage, fences)
    Blend,  ///< alpha blended, sorted back-to-front (glass, effects)
};

struct MaterialDesc {
    Color baseColor = colors::white; ///< multiplied with baseColorTexture
    float metallic = 0.0f;
    float roughness = 0.7f;
    Color emissive = colors::transparent;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f; ///< Mask mode only

    TextureRef baseColorTexture{};         ///< sRGB
    TextureRef metallicRoughnessTexture{}; ///< linear; G=roughness, B=metallic
    TextureRef normalTexture{};            ///< linear
    TextureRef occlusionTexture{};         ///< linear, R channel
    TextureRef emissiveTexture{};          ///< sRGB
};

} // namespace rendy
