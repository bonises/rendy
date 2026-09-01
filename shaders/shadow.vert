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
    uint morphWeightBase;
    uint morphDeltaBase;
    uint morphTargetCount;
    uint meshVertexBase;
    uint meshVertexCount;
} pc;

void main() {
    vec3 position = inPosition;
    vec3 unusedNormal = vec3(0.0);
    if (pc.morphTargetCount > 0u)
        rendyApplyMorphs(pc.morphTargetCount, pc.morphWeightBase, pc.morphDeltaBase,
                         pc.meshVertexCount, uint(gl_VertexIndex) - pc.meshVertexBase,
                         position, unusedNormal);
    const mat4 model = rendyModelMatrix(pc.transformIndex, pc.jointBase, inJoints, inWeights,
                                        gl_InstanceIndex);
    gl_Position = pc.lightViewProj * model * vec4(position, 1.0);
}
