#include "rendy/scene/primitives.hpp"

#include <cmath>
#include <cstdint>

namespace rendy::primitives {
namespace {

// Tangent along +u direction; good enough analytically for these shapes.
Vertex makeVertex(Vec3 position, Vec3 normal, Vec4 tangent, Vec2 uv) {
    return Vertex{position, normal, tangent, uv};
}

void addQuadFace(MeshData& mesh, Vec3 origin, Vec3 uAxis, Vec3 vAxis, Vec3 normal) {
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    const Vec4 tangent{glm::normalize(uAxis), 1.0f};
    mesh.vertices.push_back(makeVertex(origin, normal, tangent, {0.0f, 1.0f}));
    mesh.vertices.push_back(makeVertex(origin + uAxis, normal, tangent, {1.0f, 1.0f}));
    mesh.vertices.push_back(makeVertex(origin + uAxis + vAxis, normal, tangent, {1.0f, 0.0f}));
    mesh.vertices.push_back(makeVertex(origin + vAxis, normal, tangent, {0.0f, 0.0f}));
    // CCW seen from the normal side.
    mesh.indices.insert(mesh.indices.end(),
                        {base, base + 1, base + 2, base, base + 2, base + 3});
}

} // namespace

MeshData cube(Vec3 size) {
    MeshData mesh;
    const Vec3 h = size * 0.5f;
    // +x, -x, +y, -y, +z, -z
    addQuadFace(mesh, {h.x, -h.y, h.z}, {0, 0, -2 * h.z}, {0, 2 * h.y, 0}, {1, 0, 0});
    addQuadFace(mesh, {-h.x, -h.y, -h.z}, {0, 0, 2 * h.z}, {0, 2 * h.y, 0}, {-1, 0, 0});
    addQuadFace(mesh, {-h.x, h.y, h.z}, {2 * h.x, 0, 0}, {0, 0, -2 * h.z}, {0, 1, 0});
    addQuadFace(mesh, {-h.x, -h.y, -h.z}, {2 * h.x, 0, 0}, {0, 0, 2 * h.z}, {0, -1, 0});
    addQuadFace(mesh, {-h.x, -h.y, h.z}, {2 * h.x, 0, 0}, {0, 2 * h.y, 0}, {0, 0, 1});
    addQuadFace(mesh, {h.x, -h.y, -h.z}, {-2 * h.x, 0, 0}, {0, 2 * h.y, 0}, {0, 0, -1});
    mesh.boundsRadius = glm::length(h);
    return mesh;
}

MeshData sphere(float radius, int segments, int rings) {
    MeshData mesh;
    for (int ring = 0; ring <= rings; ++ring) {
        const float v = static_cast<float>(ring) / static_cast<float>(rings);
        const float phi = v * Pi; // 0 at top
        for (int segment = 0; segment <= segments; ++segment) {
            const float u = static_cast<float>(segment) / static_cast<float>(segments);
            const float theta = u * TwoPi;
            const Vec3 normal{std::sin(phi) * std::cos(theta), std::cos(phi),
                              std::sin(phi) * std::sin(theta)};
            const Vec3 tangent{-std::sin(theta), 0.0f, std::cos(theta)};
            mesh.vertices.push_back(
                makeVertex(normal * radius, normal, {tangent, 1.0f}, {u, v}));
        }
    }
    const auto stride = static_cast<uint32_t>(segments + 1);
    for (int ring = 0; ring < rings; ++ring) {
        for (int segment = 0; segment < segments; ++segment) {
            const uint32_t a = static_cast<uint32_t>(ring) * stride + static_cast<uint32_t>(segment);
            const uint32_t b = a + stride;
            mesh.indices.insert(mesh.indices.end(), {a, a + 1, b, b, a + 1, b + 1});
        }
    }
    mesh.boundsRadius = radius;
    return mesh;
}

MeshData plane(Vec2 size) {
    MeshData mesh;
    const Vec2 h = size * 0.5f;
    addQuadFace(mesh, {-h.x, 0.0f, h.y}, {2 * h.x, 0, 0}, {0, 0, -2 * h.y}, {0, 1, 0});
    mesh.boundsRadius = glm::length(h);
    return mesh;
}

namespace {

// Shared side-wall generator for cylinder/cone. topRadius = 0 gives a cone.
void addLatheSides(MeshData& mesh, float bottomRadius, float topRadius, float height,
                   int segments) {
    const float halfH = height * 0.5f;
    const float slope = (bottomRadius - topRadius) / height;
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    for (int segment = 0; segment <= segments; ++segment) {
        const float u = static_cast<float>(segment) / static_cast<float>(segments);
        const float theta = u * TwoPi;
        const float c = std::cos(theta);
        const float s = std::sin(theta);
        const Vec3 normal = glm::normalize(Vec3{c, slope, s});
        const Vec4 tangent{-s, 0.0f, c, 1.0f};
        mesh.vertices.push_back(
            makeVertex({c * bottomRadius, -halfH, s * bottomRadius}, normal, tangent, {u, 1.0f}));
        mesh.vertices.push_back(
            makeVertex({c * topRadius, halfH, s * topRadius}, normal, tangent, {u, 0.0f}));
    }
    for (int segment = 0; segment < segments; ++segment) {
        const uint32_t a = base + static_cast<uint32_t>(segment) * 2;
        mesh.indices.insert(mesh.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
    }
}

void addCap(MeshData& mesh, float radius, float y, bool up, int segments) {
    const auto center = static_cast<uint32_t>(mesh.vertices.size());
    const Vec3 normal{0.0f, up ? 1.0f : -1.0f, 0.0f};
    mesh.vertices.push_back(makeVertex({0.0f, y, 0.0f}, normal, {1, 0, 0, 1}, {0.5f, 0.5f}));
    for (int segment = 0; segment <= segments; ++segment) {
        const float theta =
            static_cast<float>(segment) / static_cast<float>(segments) * TwoPi;
        const float c = std::cos(theta);
        const float s = std::sin(theta);
        mesh.vertices.push_back(makeVertex({c * radius, y, s * radius}, normal, {1, 0, 0, 1},
                                           {0.5f + 0.5f * c, 0.5f + 0.5f * s}));
    }
    for (int segment = 0; segment < segments; ++segment) {
        const uint32_t a = center + 1 + static_cast<uint32_t>(segment);
        if (up)
            mesh.indices.insert(mesh.indices.end(), {center, a + 1, a});
        else
            mesh.indices.insert(mesh.indices.end(), {center, a, a + 1});
    }
}

} // namespace

MeshData cylinder(float radius, float height, int segments) {
    MeshData mesh;
    addLatheSides(mesh, radius, radius, height, segments);
    addCap(mesh, radius, height * 0.5f, true, segments);
    addCap(mesh, radius, -height * 0.5f, false, segments);
    mesh.boundsRadius = std::sqrt(radius * radius + height * height * 0.25f);
    return mesh;
}

MeshData cone(float radius, float height, int segments) {
    MeshData mesh;
    addLatheSides(mesh, radius, 0.0f, height, segments);
    addCap(mesh, radius, -height * 0.5f, false, segments);
    mesh.boundsRadius = std::sqrt(radius * radius + height * height * 0.25f);
    return mesh;
}

MeshData capsule(float radius, float cylinderHeight, int segments, int rings) {
    MeshData mesh;
    const float halfH = cylinderHeight * 0.5f;
    // Hemisphere rings: top (phi 0..pi/2 shifted up), sides, bottom.
    for (int ring = 0; ring <= 2 * rings + 1; ++ring) {
        float phi;
        float yOffset;
        if (ring <= rings) { // top hemisphere
            phi = static_cast<float>(ring) / static_cast<float>(rings) * HalfPi;
            yOffset = halfH;
        } else { // bottom hemisphere
            phi = HalfPi +
                  static_cast<float>(ring - rings - 1) / static_cast<float>(rings) * HalfPi;
            yOffset = -halfH;
        }
        const float v = static_cast<float>(ring) / static_cast<float>(2 * rings + 1);
        for (int segment = 0; segment <= segments; ++segment) {
            const float u = static_cast<float>(segment) / static_cast<float>(segments);
            const float theta = u * TwoPi;
            const Vec3 normal{std::sin(phi) * std::cos(theta), std::cos(phi),
                              std::sin(phi) * std::sin(theta)};
            const Vec3 position = normal * radius + Vec3{0.0f, yOffset, 0.0f};
            const Vec3 tangent{-std::sin(theta), 0.0f, std::cos(theta)};
            mesh.vertices.push_back(makeVertex(position, normal, {tangent, 1.0f}, {u, v}));
        }
    }
    const auto stride = static_cast<uint32_t>(segments + 1);
    for (int ring = 0; ring < 2 * rings + 1; ++ring) {
        for (int segment = 0; segment < segments; ++segment) {
            const uint32_t a = static_cast<uint32_t>(ring) * stride + static_cast<uint32_t>(segment);
            const uint32_t b = a + stride;
            mesh.indices.insert(mesh.indices.end(), {a, a + 1, b, b, a + 1, b + 1});
        }
    }
    mesh.boundsRadius = halfH + radius;
    return mesh;
}

MeshData torus(float majorRadius, float minorRadius, int segments, int sides) {
    MeshData mesh;
    for (int segment = 0; segment <= segments; ++segment) {
        const float u = static_cast<float>(segment) / static_cast<float>(segments);
        const float theta = u * TwoPi;
        const Vec3 ringCenter{std::cos(theta) * majorRadius, 0.0f,
                              std::sin(theta) * majorRadius};
        const Vec3 ringDir = glm::normalize(ringCenter);
        for (int side = 0; side <= sides; ++side) {
            const float v = static_cast<float>(side) / static_cast<float>(sides);
            const float phi = v * TwoPi;
            const Vec3 normal = ringDir * std::cos(phi) + Vec3{0.0f, std::sin(phi), 0.0f};
            const Vec3 position = ringCenter + normal * minorRadius;
            const Vec3 tangent{-std::sin(theta), 0.0f, std::cos(theta)};
            mesh.vertices.push_back(makeVertex(position, normal, {tangent, 1.0f}, {u, v}));
        }
    }
    const auto stride = static_cast<uint32_t>(sides + 1);
    for (int segment = 0; segment < segments; ++segment) {
        for (int side = 0; side < sides; ++side) {
            const uint32_t a = static_cast<uint32_t>(segment) * stride + static_cast<uint32_t>(side);
            const uint32_t b = a + stride;
            mesh.indices.insert(mesh.indices.end(), {a, a + 1, b, b, a + 1, b + 1});
        }
    }
    mesh.boundsRadius = majorRadius + minorRadius;
    return mesh;
}

} // namespace rendy::primitives
