#version 460
#extension GL_EXT_nonuniform_qualifier : require

// Analytic rounded-box SDF gives fill, border ring, and anti-aliasing from
// one distance value. Clipping is shader-side: scissor never breaks batches.

struct Quad {
    vec4 rect;
    vec4 uvRect;
    vec4 color;
    vec4 radii;
    vec4 borderColor;
    vec4 info; // x=borderWidth, y=textureIndex, z=clipIndex, w=kind
};

const float KIND_SOLID = 0.0;
const float KIND_IMAGE = 1.0;
const float KIND_TEXT = 2.0;

layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(std430, set = 1, binding = 0) readonly buffer Quads { Quad quads[]; };
layout(std430, set = 1, binding = 1) readonly buffer Clips { vec4 clips[]; };

layout(push_constant) uniform PC { vec2 viewport; } pc;

layout(location = 0) in vec2 vLocal;
layout(location = 1) in flat int vIndex;
layout(location = 0) out vec4 fragColor;

float roundedBoxSDF(vec2 p, vec2 halfSize, vec4 radii) {
    // radii = (tl, tr, br, bl); pick by quadrant of p (origin at center).
    float r = (p.x < 0.0) ? ((p.y < 0.0) ? radii.x : radii.w)
                          : ((p.y < 0.0) ? radii.y : radii.z);
    // CSS semantics: a radius never exceeds half the box (999px = pill).
    r = min(r, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - halfSize + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

void main() {
    Quad q = quads[vIndex];

    // Shader-side clip with a half-pixel AA edge.
    vec2 screen = q.rect.xy + vLocal;
    vec4 clip = clips[int(q.info.z)];
    float clipDist = max(max(clip.x - screen.x, screen.x - clip.z),
                         max(clip.y - screen.y, screen.y - clip.w));
    float clipAlpha = clamp(0.5 - clipDist, 0.0, 1.0);
    if (clipAlpha <= 0.0) discard;

    vec2 halfSize = q.rect.zw * 0.5;
    float dist = roundedBoxSDF(vLocal - halfSize, halfSize, q.radii);
    float coverage = clamp(0.5 - dist, 0.0, 1.0);
    if (coverage <= 0.0) discard;

    vec4 fill = vec4(srgbToLinear(q.color.rgb), q.color.a);
    float kind = q.info.w;
    if (kind == KIND_IMAGE) {
        vec2 uv = mix(q.uvRect.xy, q.uvRect.zw, vLocal / q.rect.zw);
        fill *= texture(textures[nonuniformEXT(int(q.info.y))], uv);
    } else if (kind == KIND_TEXT) {
        vec2 uv = mix(q.uvRect.xy, q.uvRect.zw, vLocal / q.rect.zw);
        fill.a *= texture(textures[nonuniformEXT(int(q.info.y))], uv).r;
    }

    float borderWidth = q.info.x;
    if (borderWidth > 0.0) {
        // Inner region coverage: 1 inside (dist < -borderWidth), 0 on the ring.
        float inner = clamp(0.5 - (dist + borderWidth), 0.0, 1.0);
        vec4 border = vec4(srgbToLinear(q.borderColor.rgb), q.borderColor.a);
        fill = mix(border, fill, inner);
    }

    fragColor = vec4(fill.rgb, fill.a * coverage * clipAlpha);
}
