#pragma once

/// \file material.hpp
/// PBR metallic-roughness materials, matching glTF 2.0 semantics.

#include "../core/color.hpp"
#include "../gpu/texture.hpp"

namespace rendy {

struct MaterialDesc {
    Color baseColor = colors::white; ///< multiplied with baseColorTexture
    float metallic = 0.0f;
    float roughness = 0.7f;
    Color emissive = colors::transparent;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;

    TextureRef baseColorTexture{};         ///< sRGB
    TextureRef metallicRoughnessTexture{}; ///< linear; G=roughness, B=metallic
    TextureRef normalTexture{};            ///< linear
    TextureRef occlusionTexture{};         ///< linear, R channel
    TextureRef emissiveTexture{};          ///< sRGB
};

} // namespace rendy
