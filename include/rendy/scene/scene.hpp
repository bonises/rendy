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
#include <string_view>
#include <vector>

namespace rendy {

namespace detail {
struct SceneImpl;
}

struct NodeId {
    uint32_t index = UINT32_MAX;
    [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
};

/// Handle for a local reflection probe (see Scene::addReflectionProbe).
struct ReflectionProbe {
    uint32_t index = UINT32_MAX;
    [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
};

/// A local reflection probe: the scene is captured from `position` into a
/// cubemap, and surfaces inside [boxMin, boxMax] use it for specular
/// reflections with parallax correction against the box (great for rooms —
/// reflections stick to the walls instead of floating at infinity).
struct ReflectionProbeDesc {
    Vec3 position{0.0f};    ///< capture point (typically the box center)
    Vec3 boxMin{-1.0f};     ///< world-space influence/projection box
    Vec3 boxMax{1.0f};
    float fade = 0.5f;      ///< edge fade toward the global environment, world units
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
    /// Frees a mesh's GPU space for reuse by later createMesh/addMesh calls.
    /// Nodes still using it lose their mesh (with a warning). The handle
    /// becomes inert but its id is recycled — drop it after this call.
    void destroyMesh(MeshHandle mesh);
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
    // NodeIds are plain indices: out-of-range or removed ids degrade safely
    // (setters no-op, reference getters return an inert dummy), but an id
    // from ANOTHER Scene cannot be detected — keep ids with their scene.

    [[nodiscard]] bool validNode(NodeId id) const;
    [[nodiscard]] NodeRef node(NodeId id) { return NodeRef(this, id); }
    [[nodiscard]] Transform& transform(NodeId id);
    /// Valid for nodes created with addLight.
    [[nodiscard]] Light& light(NodeId id);
    void setMaterial(NodeId node, MaterialHandle material);
    /// Morph target (shape key) weights for a mesh node — procedural
    /// control; glTF weight animations drive this automatically.
    void setMorphWeights(NodeId node, std::vector<float> weights);

    /// Flat ambient light (linear-ish sRGB color, small values look right).
    /// Used when no environment is set.
    void setAmbient(Color color);

    /// Load an equirectangular .hdr as the environment: skybox background +
    /// image-based diffuse/specular lighting. Baking blocks for a moment.
    Result<void> setEnvironment(const std::string& hdrPath, float intensity = 1.0f);
    void setEnvironmentIntensity(float intensity);
    /// Back to flat ambient + no skybox.
    void clearEnvironment();

    /// Adds a local reflection probe (max 8 per scene). Probes contribute
    /// nothing until bakeReflectionProbes() captures them. Returns an
    /// invalid handle when the cap is reached or the box is degenerate.
    ReflectionProbe addReflectionProbe(const ReflectionProbeDesc& desc);
    void removeReflectionProbe(ReflectionProbe probe);
    /// Captures every probe: renders the scene (opaque + skybox, no
    /// transparents) from each probe position and GGX-prefilters the result.
    /// Blocking GPU work — call after scene setup, or again when static
    /// geometry/lighting changes.
    void bakeReflectionProbes();

    /// Loads a .gltf/.glb file as a child hierarchy. Returns the root node.
    /// Imports meshes, PBR materials, textures, skins and animations.
    Result<NodeId> loadGltf(const std::string& path);

    // ---- animation -------------------------------------------------------

    struct AnimationHandle {
        uint32_t index = UINT32_MAX;
        [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
    };

    /// Names of all animations loaded so far (glTF clips), in load order.
    [[nodiscard]] std::vector<std::string> animationNames() const;
    [[nodiscard]] AnimationHandle findAnimation(std::string_view name) const;

    void playAnimation(AnimationHandle animation, bool loop = true, float speed = 1.0f);
    /// Convenience: play by clip name (no-op with a warning if missing).
    void playAnimation(std::string_view name, bool loop = true, float speed = 1.0f);
    void stopAnimation(AnimationHandle animation);
    void stopAllAnimations();
    [[nodiscard]] bool animationPlaying(AnimationHandle animation) const;

    /// Blend weight for a playing clip (weights are relative — they're
    /// normalized across clips animating the same node).
    void setAnimationWeight(AnimationHandle animation, float weight);
    /// Smoothly fade `to` in over `fadeSeconds` while fading every other
    /// playing clip out (they stop at weight 0). The go-to way to switch
    /// between e.g. Walk and Run without a pop.
    void crossfadeAnimation(AnimationHandle to, float fadeSeconds, bool loop = true,
                            float speed = 1.0f);
    void crossfadeAnimation(std::string_view name, float fadeSeconds, bool loop = true,
                            float speed = 1.0f);

    /// Advance playing clips and write the sampled values into node
    /// transforms. Call once per frame before drawing.
    void updateAnimations(float dt);

    /// Rough bounding-sphere radius of a subtree (bind pose) — handy for
    /// framing a camera around a freshly loaded model.
    [[nodiscard]] float approximateRadius(NodeId root);

private:
    friend struct detail::SceneImpl;
    friend struct detail::AppImpl;
    friend class Frame;
    std::unique_ptr<detail::SceneImpl> impl_;
};

} // namespace rendy
