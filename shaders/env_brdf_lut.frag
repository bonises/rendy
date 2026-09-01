#version 460
#extension GL_GOOGLE_include_directive : require

// Split-sum BRDF integration LUT: x = NoV, y = roughness → (scale, bias).

#include "env_common.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

float geometrySmithIBL(float NoV, float NoL, float roughness) {
    const float k = roughness * roughness / 2.0;
    const float gv = NoV / (NoV * (1.0 - k) + k);
    const float gl = NoL / (NoL * (1.0 - k) + k);
    return gv * gl;
}

void main() {
    const float NoV = max(vUV.x, 1e-3);
    const float roughness = vUV.y;
    const vec3 viewDir = vec3(sqrt(1.0 - NoV * NoV), 0.0, NoV);
    const vec3 normal = vec3(0.0, 0.0, 1.0);

    const uint kSamples = 1024u;
    float scale = 0.0;
    float bias = 0.0;
    for (uint i = 0u; i < kSamples; ++i) {
        const vec2 xi = hammersley(i, kSamples);
        const vec3 halfway = importanceSampleGGX(xi, roughness);
        const vec3 lightDir = normalize(2.0 * dot(viewDir, halfway) * halfway - viewDir);
        const float NoL = max(lightDir.z, 0.0);
        if (NoL > 0.0) {
            const float NoH = max(halfway.z, 0.0);
            const float VoH = max(dot(viewDir, halfway), 0.0);
            const float g = geometrySmithIBL(NoV, NoL, roughness);
            const float gVis = g * VoH / max(NoH * NoV, 1e-4);
            const float fc = pow(1.0 - VoH, 5.0);
            scale += (1.0 - fc) * gVis;
            bias += fc * gVis;
        }
    }
    fragColor = vec4(scale / float(kSamples), bias / float(kSamples), 0.0, 1.0);
}
