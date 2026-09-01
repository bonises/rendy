#version 460
#extension GL_GOOGLE_include_directive : require

// Equirectangular HDR → one cubemap face.

#include "env_common.glsl"

layout(set = 0, binding = 0) uniform sampler2D equirect;
layout(push_constant) uniform PC {
    uint face;
    float roughness; // unused here
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

void main() {
    const vec3 dir = cubeFaceDir(pc.face, vUV * 2.0 - 1.0);
    vec3 color = texture(equirect, equirectUv(dir)).rgb;
    // Clamp fireflies (tiny suns explode the prefilter otherwise).
    color = min(color, vec3(500.0));
    fragColor = vec4(color, 1.0);
}
