#version 460
#extension GL_GOOGLE_include_directive : require

// Environment background: reconstruct the view ray per pixel and sample the
// cube. Drawn at far depth after opaque geometry.

#include "scene_common.glsl"

layout(set = 1, binding = 8) uniform samplerCube environmentMap;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

void main() {
    // NDC → world ray using the inverse rotation part of the view.
    const vec2 ndc = vUV * 2.0 - 1.0;
    const vec4 nearPoint = frame.invViewProj * vec4(ndc, 0.1, 1.0);
    const vec4 farPoint = frame.invViewProj * vec4(ndc, 0.9, 1.0);
    const vec3 dir = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);
    const float intensity = frame.ambient.w;
    fragColor = vec4(texture(environmentMap, dir).rgb * intensity, 1.0);
}
