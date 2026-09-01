#include "rendy/scene/scene.hpp"

#include "app/app_impl.hpp"
#include "scene/scene_impl.hpp"

#include <cmath>

namespace rendy {
namespace {

// Material factors are authored as sRGB colors but shade in linear space.
float srgbToLinear(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
Vec3 srgbToLinear(Color c) {
    return {srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b)};
}

detail::GpuMaterial toGpu(const MaterialDesc& desc) {
    detail::GpuMaterial material{};
    material.baseColorFactor = Vec4{srgbToLinear(desc.baseColor), desc.baseColor.a};
    material.emissiveMetallic = Vec4{srgbToLinear(desc.emissive) *
                                         (desc.emissive.a > 0.0f ? desc.emissive.a : 1.0f),
                                     desc.metallic};
    material.params = Vec4{desc.roughness, desc.normalScale, desc.occlusionStrength, 0.0f};
    material.maps = {desc.baseColorTexture.index, desc.metallicRoughnessTexture.index,
                     desc.normalTexture.index, desc.emissiveTexture.index};
    material.maps2 = {desc.occlusionTexture.index, 0u, 0u, 0u};
    return material;
}

} // namespace

Scene::Scene(App& app) : impl_(std::make_unique<detail::SceneImpl>()) {
    impl_->app = app.impl_.get();
    impl_->meshes =
        std::make_unique<detail::MeshStore>(*impl_->app->gpu, *impl_->app->uploader);
    // Material 0: neutral default.
    impl_->materials.push_back(toGpu(MaterialDesc{.baseColor = Color::rgb(0xCCCCCC)}));
}

Scene::Scene(Scene&&) noexcept = default;
Scene& Scene::operator=(Scene&&) noexcept = default;
Scene::~Scene() {
    if (impl_ != nullptr && impl_->app != nullptr && impl_->app->gpu) {
        // Mesh buffers may be referenced by in-flight frames.
        impl_->app->gpu->waitIdle();
    }
}

MeshHandle Scene::createMesh(const MeshData& data) { return impl_->meshes->add(data); }

MaterialHandle Scene::createMaterial(const MaterialDesc& desc) {
    impl_->materials.push_back(toGpu(desc));
    return MaterialHandle{static_cast<uint32_t>(impl_->materials.size() - 1)};
}

NodeId Scene::addMesh(const MeshData& data, MaterialHandle material,
                      const Transform& transform) {
    return addMesh(createMesh(data), material, transform);
}

NodeId Scene::addMesh(MeshHandle mesh, MaterialHandle material, const Transform& transform) {
    detail::SceneNode node;
    node.local = transform;
    node.mesh = mesh;
    node.material = material;
    impl_->nodes.push_back(node);
    return NodeId{static_cast<uint32_t>(impl_->nodes.size() - 1)};
}

NodeId Scene::addNode(const Transform& transform, NodeId parent) {
    detail::SceneNode node;
    node.local = transform;
    node.parent = parent.valid() ? parent.index : UINT32_MAX;
    impl_->nodes.push_back(node);
    return NodeId{static_cast<uint32_t>(impl_->nodes.size() - 1)};
}

NodeId Scene::addLight(const Light& light, const Transform& transform) {
    detail::SceneNode node;
    node.local = transform;
    node.lightIndex = static_cast<int32_t>(impl_->lights.size());
    impl_->nodes.push_back(node);
    const NodeId id{static_cast<uint32_t>(impl_->nodes.size() - 1)};
    impl_->lights.push_back(light);
    impl_->lightNodes.push_back(id.index);
    return id;
}

void Scene::setParent(NodeId child, NodeId parent) {
    if (!child.valid()) return;
    // Reject cycles.
    for (uint32_t cursor = parent.index; cursor != UINT32_MAX;
         cursor = impl_->nodes[cursor].parent)
        if (cursor == child.index) return;
    impl_->nodes[child.index].parent = parent.valid() ? parent.index : UINT32_MAX;
}

void Scene::removeNode(NodeId node) {
    if (!node.valid() || node.index >= impl_->nodes.size()) return;
    impl_->nodes[node.index].alive = false;
    for (uint32_t i = 0; i < impl_->nodes.size(); ++i)
        if (impl_->nodes[i].parent == node.index) removeNode(NodeId{i});
}

Transform& Scene::transform(NodeId id) { return impl_->nodes[id.index].local; }

Light& Scene::light(NodeId id) {
    return impl_->lights[static_cast<size_t>(impl_->nodes[id.index].lightIndex)];
}

void Scene::setMaterial(NodeId node, MaterialHandle material) {
    impl_->nodes[node.index].material = material;
}

void Scene::setAmbient(Color color) { impl_->ambient = color; }

Result<NodeId> Scene::loadGltf(const std::string& path) {
    return err("glTF loading lands in M7 (tried to load '{}')", path);
}

// ------------------------------------------------------------------ NodeRef

NodeRef& NodeRef::setPosition(Vec3 position) {
    scene_->transform(id_).position = position;
    return *this;
}
NodeRef& NodeRef::setRotation(Quat rotation) {
    scene_->transform(id_).rotation = rotation;
    return *this;
}
NodeRef& NodeRef::setScale(Vec3 scale) {
    scene_->transform(id_).scale = scale;
    return *this;
}
NodeRef& NodeRef::rotateY(float radians) { return rotate(radians, {0.0f, 1.0f, 0.0f}); }
NodeRef& NodeRef::rotate(float radians, Vec3 axis) {
    Transform& t = scene_->transform(id_);
    t.rotation = glm::normalize(glm::angleAxis(radians, glm::normalize(axis)) * t.rotation);
    return *this;
}
Transform& NodeRef::transform() { return scene_->transform(id_); }

} // namespace rendy
