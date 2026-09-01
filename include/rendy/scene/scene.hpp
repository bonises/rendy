#pragma once

/// \file scene.hpp
/// A flat, handle-based 3D scene: nodes with transforms (parenting supported,
/// no OO scene graph), meshes, PBR materials, and lights. Draw with
/// `frame.draw(scene, camera)`.

#include "../app/app.hpp"
#include "../core/result.hpp"
#include "camera.hpp"
#include "light.hpp"
#include "material.hpp"
#include "mesh.hpp"

#include <memory>
#include <string>

namespace rendy {

namespace detail {
struct SceneImpl;
}

struct NodeId {
    uint32_t index = UINT32_MAX;
    [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
};

class Scene;

/// Convenience proxy: `scene.node(id).rotateY(dt)`.
class NodeRef {
public:
    NodeRef& setPosition(Vec3 position);
    NodeRef& setRotation(Quat rotation);
    NodeRef& setScale(Vec3 scale);
    NodeRef& setScale(float uniform) { return setScale(Vec3{uniform}); }
    NodeRef& rotateY(float radians);
    NodeRef& rotate(float radians, Vec3 axis);
    [[nodiscard]] Transform& transform();

private:
    friend class Scene;
    NodeRef(Scene* scene, NodeId id) : scene_(scene), id_(id) {}
    Scene* scene_;
    NodeId id_;
};

class Scene {
public:
    explicit Scene(App& app);
    Scene(Scene&&) noexcept;
    Scene& operator=(Scene&&) noexcept;
    ~Scene();

    // ---- content ---------------------------------------------------------

    MeshHandle createMesh(const MeshData& data);
    MaterialHandle createMaterial(const MaterialDesc& desc);
    /// Plain white-ish default material (id 0).
    [[nodiscard]] MaterialHandle defaultMaterial() const { return {0}; }

    /// Node with a mesh instance. Creates the GPU mesh from `data`.
    NodeId addMesh(const MeshData& data, MaterialHandle material, const Transform& transform = {});
    NodeId addMesh(MeshHandle mesh, MaterialHandle material, const Transform& transform = {});
    /// Empty node, useful as a parent.
    NodeId addNode(const Transform& transform = {}, NodeId parent = {});
    NodeId addLight(const Light& light, const Transform& transform = {});

    void setParent(NodeId child, NodeId parent);
    void removeNode(NodeId node);

    // ---- access ----------------------------------------------------------

    [[nodiscard]] NodeRef node(NodeId id) { return NodeRef(this, id); }
    [[nodiscard]] Transform& transform(NodeId id);
    /// Valid for nodes created with addLight.
    [[nodiscard]] Light& light(NodeId id);
    void setMaterial(NodeId node, MaterialHandle material);

    /// Flat ambient light (linear-ish sRGB color, small values look right).
    void setAmbient(Color color);

    /// Loads a .gltf/.glb file as a child hierarchy. Returns the root node.
    Result<NodeId> loadGltf(const std::string& path);

private:
    friend struct detail::SceneImpl;
    friend struct detail::AppImpl;
    friend class Frame;
    std::unique_ptr<detail::SceneImpl> impl_;
};

} // namespace rendy
