#version 460
#extension GL_GOOGLE_include_directive : require

// GGX-prefiltered specular environment, one face at one roughness (mip).

#include "env_common.glsl"

layout(set = 0, binding = 0) uniform samplerCube environment;
layout(push_constant) uniform PC {
    uint face;
    float roughness;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

void main() {
    const vec3 normal = cubeFaceDir(pc.face, vUV * 2.0 - 1.0);
    const mat3 basis = tangentBasis(normal);
    const vec3 viewDir = normal; // split-sum: N = V = R

    const uint kSamples = 512u;
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < kSamples; ++i) {
        const vec2 xi = hammersley(i, kSamples);
        const vec3 halfway = basis * importanceSampleGGX(xi, pc.roughness);
        const vec3 lightDir = normalize(2.0 * dot(viewDir, halfway) * halfway - viewDir);
        const float NoL = dot(normal, lightDir);
        if (NoL > 0.0) {
            // Sample a blurred mip to kill fireflies at high roughness.
            const float lod = pc.roughness == 0.0 ? 0.0 : 2.0;
            sum += textureLod(environment, lightDir, lod).rgb * NoL;
            weight += NoL;
        }
    }
    fragColor = vec4(sum / max(weight, 1e-4), 1.0);
}
