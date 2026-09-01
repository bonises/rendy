#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent; // xyz tangent, w handedness
layout(location = 3) in vec2 inUV;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;

layout(push_constant) uniform PC {
    uint transformIndex;
    uint materialIndex;
    uint jointBase; // NO_JOINTS = unskinned
    uint morphWeightBase;
    uint morphDeltaBase;
    uint morphTargetCount; // 0 = no morphs
    uint meshVertexBase;
    uint meshVertexCount;
} pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUV;

void main() {
    vec3 position = inPosition;
    vec3 normal = inNormal;
    if (pc.morphTargetCount > 0u)
        rendyApplyMorphs(pc.morphTargetCount, pc.morphWeightBase, pc.morphDeltaBase,
                         pc.meshVertexCount, uint(gl_VertexIndex) - pc.meshVertexBase,
                         position, normal);

    const mat4 model = rendyModelMatrix(pc.transformIndex, pc.jointBase, inJoints, inWeights,
                                        gl_InstanceIndex);
    const vec4 worldPos = model * vec4(position, 1.0);
    vWorldPos = worldPos.xyz;

    // Normal matrix; fine for rigid + uniformly scaled transforms, and close
    // enough for the rest in v1.
    const mat3 normalMatrix = transpose(inverse(mat3(model)));
    vNormal = normalMatrix * normalize(normal);
    vTangent = vec4(mat3(model) * inTangent.xyz, inTangent.w);
    vUV = inUV;

    gl_Position = frame.viewProj * worldPos;
}
