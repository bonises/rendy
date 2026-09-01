#version 460
#extension GL_GOOGLE_include_directive : require

// Depth-only pass for all shadow maps. The light matrix comes via push
// constants so one pipeline serves cascades, spots and cube faces.

#include "scene_common.glsl"

layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform PC {
    mat4 lightViewProj;
    uint transformIndex;
} pc;

void main() {
    gl_Position = pc.lightViewProj * transforms[pc.transformIndex] * vec4(inPosition, 1.0);
}
