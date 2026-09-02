#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"

layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(set = 1, binding = 4) uniform sampler2DArrayShadow cascadeShadowMaps;
layout(set = 1, binding = 5) uniform sampler2DArrayShadow spotShadowMaps;
layout(set = 1, binding = 6) uniform samplerCubeArray pointShadowMaps;
layout(set = 1, binding = 9) uniform samplerCube irradianceMap;
layout(set = 1, binding = 10) uniform samplerCube prefilteredMap;
layout(set = 1, binding = 11) uniform sampler2D brdfLut;
layout(set = 1, binding = 16) uniform samplerCubeArray reflectionProbes;

layout(push_constant) uniform PC {
    uint transformIndex;
    uint materialIndex;
    uint jointBase;
    uint morphWeightBase;
    uint morphDeltaBase;
    uint morphTargetCount;
    uint meshVertexBase;
    uint meshVertexCount;
} pc;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUV;

layout(location = 0) out vec4 fragColor;

const float PI = 3.14159265359;

// ---------------------------------------------------------------- BRDF bits

vec3 fresnelSchlick(vec3 f0, float VoH) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - VoH, 0.0, 1.0), 5.0);
}

float distributionGGX(float NoH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}

float visibilitySmith(float NoV, float NoL, float roughness) {
    float a = roughness * roughness;
    float v = NoL * (NoV * (1.0 - a) + a);
    float l = NoV * (NoL * (1.0 - a) + a);
    return 0.5 / max(v + l, 1e-6);
}

vec3 sampleOrWhite(uint index, vec2 uv) {
    if (index == 0u) return vec3(1.0);
    return texture(textures[nonuniformEXT(index)], uv).rgb;
}

// ------------------------------------------------------------------ shadows

// 3x3 PCF on an array shadow map (hardware compare per tap).
float pcfArray(sampler2DArrayShadow map, vec3 uvw, float depthRef) {
    const vec2 texel = 1.0 / vec2(textureSize(map, 0).xy);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(map, vec4(uvw.xy + vec2(x, y) * texel, uvw.z, depthRef));
    return sum / 9.0;
}

// 1 = fully lit, 0 = fully shadowed.
float directionalShadow(vec3 worldPos, float NoL) {
    const uint cascadeCount = frame.counts.y;
    if (cascadeCount == 0u) return 1.0;

    // Pick cascade from view depth.
    const float viewDepth = -(frame.view * vec4(worldPos, 1.0)).z;
    uint cascade = cascadeCount - 1u;
    for (uint i = 0u; i < cascadeCount; ++i) {
        if (viewDepth < frame.cascadeSplits[i]) {
            cascade = i;
            break;
        }
    }

    const vec4 lightClip = frame.cascadeMatrices[cascade] * vec4(worldPos, 1.0);
    vec3 ndc = lightClip.xyz / lightClip.w;
    const vec2 uv = ndc.xy * 0.5 + 0.5;
    if (ndc.z <= 0.0 || ndc.z >= 1.0) return 1.0;

    const float bias = max(0.0015 * (1.0 - NoL), 0.0004) * (1.0 + float(cascade));
    return pcfArray(cascadeShadowMaps, vec3(uv, float(cascade)), ndc.z - bias);
}

float spotShadow(uint shadowIndex, vec3 worldPos, float NoL) {
    const vec4 lightClip = frame.spotMatrices[shadowIndex] * vec4(worldPos, 1.0);
    if (lightClip.w <= 0.0) return 1.0;
    vec3 ndc = lightClip.xyz / lightClip.w;
    const vec2 uv = ndc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    const float bias = max(0.002 * (1.0 - NoL), 0.0005);
    return pcfArray(spotShadowMaps, vec3(uv, float(shadowIndex)), ndc.z - bias);
}

float pointShadow(uint shadowIndex, vec3 worldPos, vec3 lightPos, float range) {
    const vec3 toFrag = worldPos - lightPos;
    const float dist = max(max(abs(toFrag.x), abs(toFrag.y)), abs(toFrag.z));
    const float nearPlane = frame.pointShadowParams.x;
    const float farPlane = max(range, 1.0);
    // Depth the cube map stored for this fragment (standard 0..1 projection).
    const float depthRef =
        farPlane / (farPlane - nearPlane) * (1.0 - nearPlane / max(dist, nearPlane));
    const float stored = texture(pointShadowMaps, vec4(toFrag, float(shadowIndex))).r;
    return stored + 0.002 >= depthRef ? 1.0 : 0.0;
}

void main() {
    const Material material = materials[pc.materialIndex];

    // ---- material inputs
    vec4 baseColor = material.baseColorFactor;
    if (material.maps.x != 0u)
        baseColor *= texture(textures[nonuniformEXT(material.maps.x)], vUV);

    // Alpha mask (params.w = cutoff, 0 = disabled).
    if (material.params.w > 0.0 && baseColor.a < material.params.w) discard;

    float metallic = material.emissiveMetallic.w;
    float roughness = material.params.x;
    if (material.maps.y != 0u) {
        // glTF: G = roughness, B = metallic.
        const vec3 mr = texture(textures[nonuniformEXT(material.maps.y)], vUV).rgb;
        roughness *= mr.g;
        metallic *= mr.b;
    }
    roughness = clamp(roughness, 0.045, 1.0);

    vec3 normal = normalize(vNormal);
    if (material.maps.z != 0u) {
        const vec3 tangent = normalize(vTangent.xyz - normal * dot(vTangent.xyz, normal));
        const vec3 bitangent = cross(normal, tangent) * vTangent.w;
        vec3 sampled = texture(textures[nonuniformEXT(material.maps.z)], vUV).xyz * 2.0 - 1.0;
        sampled.xy *= material.params.y; // normalScale
        normal = normalize(mat3(tangent, bitangent, normal) * sampled);
    }

    float occlusion = 1.0;
    if (material.maps2.x != 0u) {
        const float sampled = texture(textures[nonuniformEXT(material.maps2.x)], vUV).r;
        occlusion = mix(1.0, sampled, material.params.z);
    }

    vec3 emissive = material.emissiveMetallic.xyz;
    if (material.maps.w != 0u)
        emissive *= texture(textures[nonuniformEXT(material.maps.w)], vUV).rgb;

    const vec3 albedo = baseColor.rgb;
    const vec3 f0 = mix(vec3(0.04), albedo, metallic);
    const vec3 diffuseColor = albedo * (1.0 - metallic);

    const vec3 viewDir = normalize(frame.viewPos.xyz - vWorldPos);
    const float NoV = max(dot(normal, viewDir), 1e-4);
    // Double-sided-ish: flip the normal for back faces.
    const vec3 shadingNormal = gl_FrontFacing ? normal : -normal;

    // ---- lights (forward+: directional lights up front + this fragment's
    // cluster list for point/spot)
    const float clusterViewDepth = -(frame.view * vec4(vWorldPos, 1.0)).z;

#define SHADE_LIGHT(light)                                                                   \
    {                                                                                        \
        const uint type = uint(light.positionType.w);                                        \
        vec3 lightDir;                                                                       \
        float attenuation = 1.0;                                                             \
        if (type == LIGHT_DIRECTIONAL) {                                                     \
            lightDir = normalize(-light.positionType.xyz);                                   \
        } else {                                                                             \
            const vec3 toLight = light.positionType.xyz - vWorldPos;                         \
            const float distance = length(toLight);                                          \
            lightDir = toLight / max(distance, 1e-4);                                        \
            attenuation = 1.0 / max(distance * distance, 1e-4);                              \
            const float range = light.directionRange.w;                                      \
            if (range > 0.0) {                                                               \
                const float factor = clamp(1.0 - pow(distance / range, 4.0), 0.0, 1.0);      \
                attenuation *= factor * factor;                                              \
            }                                                                                \
            if (type == LIGHT_SPOT) {                                                        \
                const float cosAngle = dot(-lightDir, normalize(light.directionRange.xyz));  \
                attenuation *= clamp((cosAngle - light.cone.y) /                             \
                                         max(light.cone.x - light.cone.y, 1e-4),             \
                                     0.0, 1.0);                                              \
            }                                                                                \
        }                                                                                    \
        const float NoL = max(dot(shadingNormal, lightDir), 0.0);                            \
        if (NoL > 0.0 && attenuation > 0.0) {                                                \
            const float shadowIndex = light.cone.z;                                          \
            if (shadowIndex >= 0.0) {                                                        \
                if (type == LIGHT_DIRECTIONAL)                                               \
                    attenuation *= directionalShadow(vWorldPos, NoL);                        \
                else if (type == LIGHT_SPOT)                                                 \
                    attenuation *= spotShadow(uint(shadowIndex), vWorldPos, NoL);            \
                else                                                                         \
                    attenuation *= pointShadow(uint(shadowIndex), vWorldPos,                 \
                                               light.positionType.xyz,                      \
                                               light.directionRange.w);                     \
            }                                                                                \
            if (attenuation > 0.0) {                                                         \
                const vec3 halfway = normalize(viewDir + lightDir);                          \
                const float NoH = max(dot(shadingNormal, halfway), 0.0);                     \
                const float VoH = max(dot(viewDir, halfway), 0.0);                           \
                const vec3 fresnel = fresnelSchlick(f0, VoH);                                \
                const float distribution = distributionGGX(NoH, roughness);                  \
                const float visibility = visibilitySmith(NoV, NoL, roughness);               \
                const vec3 specular = fresnel * (distribution * visibility);                 \
                const vec3 diffuse = diffuseColor / PI;                                      \
                const vec3 radiance =                                                        \
                    light.colorIntensity.rgb * light.colorIntensity.w * attenuation;         \
                radianceSum += (diffuse * (1.0 - fresnel) + specular) * radiance * NoL;      \
            }                                                                                \
        }                                                                                    \
    }

    vec3 radianceSum = vec3(0.0);
    // Directional lights (always shaded, stored first).
    for (uint i = 0u; i < frame.counts.w; ++i) {
        const LightData light = lights[i];
        SHADE_LIGHT(light);
    }
    // Clustered point/spot lights.
    const uint cluster = rendyClusterIndex(gl_FragCoord.xy, clusterViewDepth);
    const uvec2 clusterRange = clusters[cluster];
    for (uint k = 0u; k < clusterRange.y; ++k) {
        const LightData light = lights[clusterLightIndices[clusterRange.x + k]];
        SHADE_LIGHT(light);
    }

    // ---- ambient: global IBL (or flat ambient) + local reflection probes
    const vec3 reflected = reflect(-viewDir, shadingNormal);
    const vec2 brdf = texture(brdfLut, vec2(NoV, roughness)).rg;
    const vec3 specularScale = f0 * brdf.x + brdf.y;

    vec3 ambient;
    vec3 baseSpecular = vec3(0.0); // what a probe replaces, weight-blended
    if (frame.counts.z == 1u) {
        // Image-based lighting: irradiance for diffuse, prefiltered + BRDF
        // LUT split-sum for specular.
        const vec3 irradiance = texture(irradianceMap, shadingNormal).rgb;
        const vec3 prefiltered =
            textureLod(prefilteredMap, reflected, roughness * frame.pointShadowParams.y).rgb;
        baseSpecular = prefiltered * specularScale * occlusion * frame.ambient.w;
        ambient = irradiance * diffuseColor * occlusion * frame.ambient.w + baseSpecular;
    } else {
        ambient = frame.ambient.rgb * albedo * occlusion;
    }

    // Local reflection probes: pick the containing box with the strongest
    // edge-fade weight, parallax-correct the reflection ray against the box,
    // and blend the probe's prefiltered capture over the global specular.
    const uint probeCount = uint(frame.pointShadowParams.z);
    float probeWeight = 0.0;
    uint probeIndex = 0u;
    for (uint p = 0u; p < probeCount; ++p) {
        if (frame.probePositions[p].w == 0.0) continue;
        const vec3 boxMin = frame.probeBoxMins[p].xyz;
        const vec3 boxMax = frame.probeBoxMaxs[p].xyz;
        if (any(lessThan(vWorldPos, boxMin)) || any(greaterThan(vWorldPos, boxMax))) continue;
        const float fade = max(frame.probeBoxMins[p].w, 1e-4);
        const vec3 edge = min(vWorldPos - boxMin, boxMax - vWorldPos);
        const float weight = clamp(min(edge.x, min(edge.y, edge.z)) / fade, 0.0, 1.0);
        if (weight > probeWeight) {
            probeWeight = weight;
            probeIndex = p;
        }
    }
    if (probeWeight > 0.0) {
        const vec3 boxMin = frame.probeBoxMins[probeIndex].xyz;
        const vec3 boxMax = frame.probeBoxMaxs[probeIndex].xyz;
        // Guard against 0-components before the slab intersection (0 * inf = NaN).
        const vec3 ray = mix(reflected, vec3(1e-5), vec3(lessThan(abs(reflected), vec3(1e-5))));
        const vec3 invRay = 1.0 / ray;
        const vec3 tMax = max((boxMin - vWorldPos) * invRay, (boxMax - vWorldPos) * invRay);
        const float hit = max(min(min(tMax.x, tMax.y), tMax.z), 0.0);
        const vec3 dir = (vWorldPos + ray * hit) - frame.probePositions[probeIndex].xyz;
        const vec3 probeSpec =
            textureLod(reflectionProbes, vec4(dir, float(probeIndex)),
                       roughness * frame.pointShadowParams.w).rgb *
            specularScale * occlusion;
        ambient += probeWeight * (probeSpec - baseSpecular);
    }
    fragColor = vec4(radianceSum + ambient + emissive, baseColor.a);
}
