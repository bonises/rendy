#pragma once

/// \file primitives.hpp
/// CPU generators for the basic shapes. All produce positions, normals,
/// tangents and UVs, centered at origin (plane/cylinder/cone sit on y = 0
/// center; capsule/sphere centered).

#include "mesh.hpp"

namespace rendy::primitives {

MeshData cube(Vec3 size = {1.0f, 1.0f, 1.0f});
/// UV sphere.
MeshData sphere(float radius = 0.5f, int segments = 48, int rings = 24);
/// Flat XZ plane facing +y, UV tiled once.
MeshData plane(Vec2 size = {1.0f, 1.0f});
MeshData cylinder(float radius = 0.5f, float height = 1.0f, int segments = 48);
MeshData cone(float radius = 0.5f, float height = 1.0f, int segments = 48);
/// Total height = cylinderHeight + 2*radius.
MeshData capsule(float radius = 0.25f, float cylinderHeight = 0.5f, int segments = 32,
                 int rings = 8);
MeshData torus(float majorRadius = 0.5f, float minorRadius = 0.15f, int segments = 48,
               int sides = 24);

} // namespace rendy::primitives
