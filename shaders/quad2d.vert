#version 460

// One instanced 4-vertex triangle strip per quad. All 2D content (solid
// rects, images, glyphs) flows through this shader in a single draw.

struct Quad {
    vec4 rect;        // x, y, w, h in pixels
    vec4 uvRect;      // u0, v0, u1, v1
    vec4 color;       // fill (sRGB, straight alpha)
    vec4 radii;       // corner radii px: tl, tr, br, bl
    vec4 borderColor; // sRGB
    vec4 info;        // x=borderWidth, y=textureIndex, z=clipIndex, w=kind
};

layout(std430, set = 1, binding = 0) readonly buffer Quads { Quad quads[]; };

layout(push_constant) uniform PC { vec2 viewport; } pc;

layout(location = 0) out vec2 vLocal;       // position inside quad, px
layout(location = 1) out flat int vIndex;

void main() {
    Quad q = quads[gl_InstanceIndex];
    vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    vLocal = corner * q.rect.zw;
    vIndex = gl_InstanceIndex;
    vec2 pos = (q.rect.xy + vLocal) / pc.viewport * 2.0 - 1.0;
    gl_Position = vec4(pos, 0.0, 1.0);
}
