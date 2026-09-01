#version 460
#extension GL_GOOGLE_include_directive : require

// Depth-only pass for all shadow maps. The light matrix comes via push
// constants so one pipeline serves cascades, spots and cube faces.

#include "scene_common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;

layout(push_constant) uniform PC {
    mat4 lightViewProj;
    uint transformIndex;
    uint jointBase;
} pc;

void main() {
    const mat4 model = rendyModelMatrix(pc.transformIndex, pc.jointBase, inJoints, inWeights,
                                        gl_InstanceIndex);
    gl_Position = pc.lightViewProj * model * vec4(inPosition, 1.0);
}
