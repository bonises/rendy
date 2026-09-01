// Shared helpers for environment baking passes.

const float ENV_PI = 3.14159265359;

// Direction for a cubemap face texel; uv in [-1, 1], faces in Vulkan order.
vec3 cubeFaceDir(uint face, vec2 uv) {
    switch (face) {
    case 0u: return normalize(vec3(1.0, -uv.y, -uv.x));  // +X
    case 1u: return normalize(vec3(-1.0, -uv.y, uv.x));  // -X
    case 2u: return normalize(vec3(uv.x, 1.0, uv.y));    // +Y
    case 3u: return normalize(vec3(uv.x, -1.0, -uv.y));  // -Y
    case 4u: return normalize(vec3(uv.x, -uv.y, 1.0));   // +Z
    default: return normalize(vec3(-uv.x, -uv.y, -1.0)); // -Z
    }
}

vec2 equirectUv(vec3 dir) {
    return vec2(atan(dir.z, dir.x) / (2.0 * ENV_PI) + 0.5, acos(clamp(dir.y, -1.0, 1.0)) / ENV_PI);
}

// Van der Corput / Hammersley for importance sampling.
float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint i, uint count) {
    return vec2(float(i) / float(count), radicalInverseVdC(i));
}

// GGX importance sample around +Z, returned in tangent space.
vec3 importanceSampleGGX(vec2 xi, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * ENV_PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

mat3 tangentBasis(vec3 normal) {
    vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
    return mat3(tangent, bitangent, normal);
}
