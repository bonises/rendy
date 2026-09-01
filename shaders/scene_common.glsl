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
    mat4 invViewProj;
    mat4 cascadeMatrices[MAX_CASCADES];
    mat4 spotMatrices[MAX_SPOT_SHADOWS];
    vec4 cascadeSplits; // view-space split depths (positive)
    vec4 viewPos;       // xyz camera position
    vec4 ambient;       // rgb flat ambient, w = environment intensity
    uvec4 counts;       // x = light count, y = cascades, z = has env, w = directional count
    vec4 pointShadowParams; // x = point shadow near plane, y = prefiltered env max mip
    vec4 clusterParams;     // x,y = tile size px, z = z-slice scale, w = z-slice bias
} frame;

layout(std430, set = 1, binding = 1) readonly buffer Transforms { mat4 transforms[]; };
layout(std430, set = 1, binding = 2) readonly buffer Materials { Material materials[]; };
layout(std430, set = 1, binding = 3) readonly buffer Lights { LightData lights[]; };
layout(std430, set = 1, binding = 7) readonly buffer Joints { mat4 jointMatrices[]; };

struct MorphDelta {
    vec4 position;
    vec4 normal;
};
layout(std430, set = 1, binding = 12) readonly buffer MorphDeltas { MorphDelta morphDeltas[]; };
layout(std430, set = 1, binding = 13) readonly buffer MorphWeights { float morphWeights[]; };
// Forward+ clusters: per-cluster (offset, count) into the light index list.
layout(std430, set = 1, binding = 14) readonly buffer Clusters { uvec2 clusters[]; };
layout(std430, set = 1, binding = 15) readonly buffer ClusterIndices { uint clusterLightIndices[]; };

const uint CLUSTER_X = 16u;
const uint CLUSTER_Y = 9u;
const uint CLUSTER_Z = 24u;

uint rendyClusterIndex(vec2 fragCoord, float viewDepth) {
    uvec2 tile = uvec2(fragCoord / frame.clusterParams.xy);
    tile = min(tile, uvec2(CLUSTER_X - 1u, CLUSTER_Y - 1u));
    const float slice =
        log2(max(viewDepth, 0.01)) * frame.clusterParams.z - frame.clusterParams.w;
    const uint z = uint(clamp(slice, 0.0, float(CLUSTER_Z - 1u)));
    return tile.x + CLUSTER_X * (tile.y + CLUSTER_Y * z);
}

const uint NO_JOINTS = 0xFFFFFFFFu;

// Applies morph target deltas (before skinning, per glTF). localVertex is
// the vertex index within its own mesh.
void rendyApplyMorphs(uint targetCount, uint weightBase, uint deltaBase, uint vertexCount,
                      uint localVertex, inout vec3 position, inout vec3 normal) {
    for (uint t = 0u; t < targetCount; ++t) {
        const float w = morphWeights[weightBase + t];
        if (w == 0.0) continue;
        const MorphDelta delta = morphDeltas[deltaBase + t * vertexCount + localVertex];
        position += w * delta.position.xyz;
        normal += w * delta.normal.xyz;
    }
}

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
