#version 460
#extension GL_EXT_nonuniform_qualifier : require

// HDR resolve → tonemap → sRGB swapchain (hardware encodes).

layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(push_constant) uniform PC {
    uint hdrTexture;
    uint tonemapper; // 0 = Khronos PBR Neutral, 1 = ACES (Narkowicz), 2 = none
    float exposure;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

// Khronos PBR Neutral: preserves hue and saturation better than filmic
// curves — colors stay true to the material's albedo.
vec3 pbrNeutral(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;

    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, newPeak * vec3(1.0), g);
}

vec3 acesNarkowicz(vec3 color) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(textures[nonuniformEXT(pc.hdrTexture)], vUV).rgb * pc.exposure;
    vec3 mapped = pc.tonemapper == 0u ? pbrNeutral(hdr)
                : pc.tonemapper == 1u ? acesNarkowicz(hdr)
                                      : hdr;
    fragColor = vec4(mapped, 1.0);
}
