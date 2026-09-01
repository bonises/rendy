#pragma once

// Scene internals: flat node array (SoA-ish), CPU material/light mirrors.

#include "scene/mesh_store.hpp"
#include "rendy/scene/light.hpp"
#include "rendy/scene/material.hpp"
#include "rendy/scene/scene.hpp"

#include <memory>
#include <vector>

namespace rendy::detail {

struct AppImpl;

// Matches shaders/scene_common.glsl `Material` (std430).
struct GpuMaterial {
    Vec4 baseColorFactor;
    Vec4 emissiveMetallic;
    Vec4 params; // roughness, normalScale, occlusionStrength, unused
    glm::uvec4 maps;  // baseColor, metallicRoughness, normal, emissive
    glm::uvec4 maps2; // occlusion
};
static_assert(sizeof(GpuMaterial) == 80);

// Matches shaders/scene_common.glsl `LightData`.
struct GpuLight {
    Vec4 positionType;
    Vec4 colorIntensity;
    Vec4 directionRange;
    Vec4 cone;
};

struct SceneNode {
    Transform local;
    uint32_t parent = UINT32_MAX;
    Mat4 world{1.0f};
    MeshHandle mesh;         // invalid = no mesh
    MaterialHandle material;
    int32_t lightIndex = -1; // into lights
    bool alive = true;
};

struct SceneImpl {
    AppImpl* app = nullptr;
    std::unique_ptr<MeshStore> meshes;
    std::vector<SceneNode> nodes;
    std::vector<Light> lights;          // authoring data; node holds transform
    std::vector<uint32_t> lightNodes;   // node index per light
    std::vector<GpuMaterial> materials; // uploaded per frame
    std::vector<AlphaMode> materialAlphaModes; // parallel to materials
    Color ambient{0.03f, 0.03f, 0.04f, 1.0f};

    /// Recompute world matrices (parents are always created before children,
    /// and setParent rejects cycles, so one forward pass works... except
    /// reparenting to a later node — handled with a resolve loop).
    void updateWorldTransforms() {
        for (size_t i = 0; i < nodes.size(); ++i) {
            SceneNode& node = nodes[i];
            if (node.parent == UINT32_MAX) {
                node.world = node.local.matrix();
            } else if (node.parent < i) {
                node.world = nodes[node.parent].world * node.local.matrix();
            } else {
                // Parent computed later this pass: resolve recursively.
                node.world = worldOf(node.parent) * node.local.matrix();
            }
        }
    }

    Mat4 worldOf(uint32_t index) {
        const SceneNode& node = nodes[index];
        if (node.parent == UINT32_MAX) return node.local.matrix();
        return worldOf(node.parent) * node.local.matrix();
    }
};

} // namespace rendy::detail
