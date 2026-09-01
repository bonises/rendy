#version 460

// Fullscreen triangle at far depth for the environment background.

layout(location = 0) out vec2 vUV;

void main() {
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 1.0, 1.0); // z = far
}
