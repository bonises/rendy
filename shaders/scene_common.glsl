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

layout(set = 1, binding = 0) uniform FrameData {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    vec4 viewPos;       // xyz camera position
    vec4 ambient;       // rgb ambient light
    uvec4 counts;       // x = light count
} frame;

layout(std430, set = 1, binding = 1) readonly buffer Transforms { mat4 transforms[]; };
layout(std430, set = 1, binding = 2) readonly buffer Materials { Material materials[]; };
layout(std430, set = 1, binding = 3) readonly buffer Lights { LightData lights[]; };
