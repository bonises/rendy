#version 460
#extension GL_GOOGLE_include_directive : require

// Cosine-convolved diffuse irradiance, one cubemap face.

#include "env_common.glsl"

layout(set = 0, binding = 0) uniform samplerCube environment;
layout(push_constant) uniform PC {
    uint face;
    float roughness; // unused
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

void main() {
    const vec3 normal = cubeFaceDir(pc.face, vUV * 2.0 - 1.0);
    const mat3 basis = tangentBasis(normal);

    vec3 irradiance = vec3(0.0);
    int samples = 0;
    const float dPhi = 2.0 * ENV_PI / 64.0;
    const float dTheta = 0.5 * ENV_PI / 16.0;
    for (float phi = 0.0; phi < 2.0 * ENV_PI; phi += dPhi) {
        for (float theta = 0.0; theta < 0.5 * ENV_PI; theta += dTheta) {
            const vec3 tangentDir =
                vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            const vec3 dir = basis * tangentDir;
            irradiance += textureLod(environment, dir, 2.0).rgb * cos(theta) * sin(theta);
            samples++;
        }
    }
    irradiance = ENV_PI * irradiance / float(samples);
    fragColor = vec4(irradiance, 1.0);
}
