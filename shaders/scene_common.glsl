// Shared per-frame data layout for the 3D passes (set 1).

struct Material {
    vec4 baseColorFactor;     // rgba
    vec4 emissiveMetallic;    // xyz emissive factor, w metallic
    vec4 params;              // x roughness, y normalScale, z occlusionStrength, w unused
    uvec4 maps;               // x baseColor, y metallicRoughness, z normal, w emissive
    uvec4 maps2;              // x occlusion, yzw unused (0 = no texture)
};

const uint LIGHT_DIRECTIONAL = 0u;
const uint LIGHT_POINT = 1u;
const uint LIGHT_SPOT = 2u;

struct LightData {
    vec4 positionType;   // xyz position (or direction for directional), w type
    vec4 colorIntensity; // rgb linear color, w intensity
    vec4 directionRange; // xyz spot/dir direction, w range (0 = unbounded)
    vec4 cone;           // x cos(inner), y cos(outer), z shadowIndex (-1 none), w unused
};

const uint MAX_CASCADES = 4u;
const uint MAX_SPOT_SHADOWS = 8u;

layout(set = 1, binding = 0) uniform FrameData {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 cascadeMatrices[MAX_CASCADES];
    mat4 spotMatrices[MAX_SPOT_SHADOWS];
    vec4 cascadeSplits; // view-space split depths (positive)
    vec4 viewPos;       // xyz camera position
    vec4 ambient;       // rgb ambient light
    uvec4 counts;       // x = light count, y = active cascades
    vec4 pointShadowParams; // x = near plane used for point shadow projs
} frame;

layout(std430, set = 1, binding = 1) readonly buffer Transforms { mat4 transforms[]; };
layout(std430, set = 1, binding = 2) readonly buffer Materials { Material materials[]; };
layout(std430, set = 1, binding = 3) readonly buffer Lights { LightData lights[]; };
layout(std430, set = 1, binding = 7) readonly buffer Joints { mat4 jointMatrices[]; };

const uint NO_JOINTS = 0xFFFFFFFFu;

// Model matrix for a (possibly skinned) vertex. Skinned meshes follow their
// skeleton and ignore the node transform, per the glTF spec.
mat4 rendyModelMatrix(uint transformIndex, uint jointBase, uvec4 joints, vec4 weights,
                      uint instanceIndex) {
    if (jointBase == NO_JOINTS) return transforms[transformIndex + instanceIndex];
    return weights.x * jointMatrices[jointBase + joints.x] +
           weights.y * jointMatrices[jointBase + joints.y] +
           weights.z * jointMatrices[jointBase + joints.z] +
           weights.w * jointMatrices[jointBase + joints.w];
}
