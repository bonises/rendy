#include "rendy/scene/scene.hpp"

#include "app/app_impl.hpp"
#include "scene/animation_sampler.hpp"
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
    material.params =
        Vec4{desc.roughness, desc.normalScale, desc.occlusionStrength,
             desc.alphaMode == AlphaMode::Mask ? desc.alphaCutoff : 0.0f};
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
    impl_->materialAlphaModes.push_back(AlphaMode::Opaque);
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
    impl_->materialAlphaModes.push_back(desc.alphaMode);
    return MaterialHandle{static_cast<uint32_t>(impl_->materials.size() - 1)};
}

NodeId Scene::addMesh(const MeshData& data, MaterialHandle material,
                      const Transform& transform) {
    return addMesh(createMesh(data), material, transform);
}

NodeId Scene::addMesh(MeshHandle mesh, MaterialHandle material, const Transform& transform) {
    if (material.id >= impl_->materials.size()) {
        log::warn("Scene::addMesh: invalid MaterialHandle {}, using default", material.id);
        material = MaterialHandle{0};
    }
    detail::SceneNode node;
    node.local = transform;
    node.baseLocal = transform;
    node.mesh = mesh;
    node.material = material;
    impl_->nodes.push_back(node);
    return NodeId{static_cast<uint32_t>(impl_->nodes.size() - 1)};
}

NodeId Scene::addNode(const Transform& transform, NodeId parent) {
    detail::SceneNode node;
    node.local = transform;
    node.baseLocal = transform;
    node.parent = parent.valid() ? parent.index : UINT32_MAX;
    impl_->nodes.push_back(node);
    return NodeId{static_cast<uint32_t>(impl_->nodes.size() - 1)};
}

NodeId Scene::addLight(const Light& light, const Transform& transform) {
    detail::SceneNode node;
    node.local = transform;
    node.baseLocal = transform;
    node.lightIndex = static_cast<int32_t>(impl_->lights.size());
    impl_->nodes.push_back(node);
    const NodeId id{static_cast<uint32_t>(impl_->nodes.size() - 1)};
    impl_->lights.push_back(light);
    impl_->lightNodes.push_back(id.index);
    return id;
}

// A NodeId is only meaningful for the Scene that produced it; foreign or
// stale ids must degrade safely, not index out of bounds.
bool Scene::validNode(NodeId id) const {
    return id.valid() && id.index < impl_->nodes.size() && impl_->nodes[id.index].alive;
}

void Scene::setParent(NodeId child, NodeId parent) {
    if (!validNode(child)) return;
    if (parent.valid() && !validNode(parent)) return;
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

Transform& Scene::transform(NodeId id) {
    if (!validNode(id)) {
        log::warn("Scene::transform: invalid NodeId {}", id.index);
        static Transform dummy;
        dummy = Transform{}; // callers may have scribbled on it
        return dummy;
    }
    return impl_->nodes[id.index].local;
}

Light& Scene::light(NodeId id) {
    if (!validNode(id) || impl_->nodes[id.index].lightIndex < 0) {
        log::warn("Scene::light: NodeId {} is not a light", id.index);
        static Light dummy;
        dummy = Light{};
        return dummy;
    }
    return impl_->lights[static_cast<size_t>(impl_->nodes[id.index].lightIndex)];
}

void Scene::setMaterial(NodeId node, MaterialHandle material) {
    if (!validNode(node)) return;
    if (material.id >= impl_->materials.size()) {
        log::warn("Scene::setMaterial: invalid MaterialHandle {}", material.id);
        return;
    }
    impl_->nodes[node.index].material = material;
}

void Scene::setMorphWeights(NodeId node, std::vector<float> weights) {
    if (!validNode(node)) return;
    impl_->nodes[node.index].morphWeights = std::move(weights);
    // Mesh primitives are children of the glTF node; mirror onto them too.
    for (uint32_t i = 0; i < impl_->nodes.size(); ++i)
        if (impl_->nodes[i].parent == node.index && impl_->nodes[i].mesh.valid())
            impl_->nodes[i].morphWeights = impl_->nodes[node.index].morphWeights;
}

void Scene::setAmbient(Color color) { impl_->ambient = color; }

Result<void> Scene::setEnvironment(const std::string& hdrPath, float intensity) {
    auto baked = detail::bakeEnvironment(*impl_->app->gpu, hdrPath);
    if (!baked) return baked.error();
    if (impl_->environment) {
        // The old maps may be referenced by in-flight frames.
        auto old = impl_->environment;
        impl_->app->frames->defer([old] {});
    }
    impl_->environment = std::move(baked).value();
    impl_->environmentIntensity = intensity;
    return {};
}

void Scene::setEnvironmentIntensity(float intensity) {
    impl_->environmentIntensity = intensity;
}

void Scene::clearEnvironment() {
    if (impl_->environment) {
        auto old = impl_->environment;
        impl_->app->frames->defer([old] {});
        impl_->environment.reset();
    }
}

// Scene::loadGltf lives in gltf.cpp.

// --------------------------------------------------------------- animation

std::vector<std::string> Scene::animationNames() const {
    std::vector<std::string> names;
    names.reserve(impl_->animations.size());
    for (const auto& animation : impl_->animations) names.push_back(animation.name);
    return names;
}

Scene::AnimationHandle Scene::findAnimation(std::string_view name) const {
    for (uint32_t i = 0; i < impl_->animations.size(); ++i)
        if (impl_->animations[i].name == name) return {i};
    return {};
}

void Scene::playAnimation(AnimationHandle handle, bool loop, float speed) {
    if (!handle.valid() || handle.index >= impl_->animations.size()) return;
    auto& animation = impl_->animations[handle.index];
    animation.playing = true;
    animation.loop = loop;
    animation.speed = speed;
    animation.time = 0.0;
    animation.weight = 1.0f;
    animation.targetWeight = 1.0f;
    animation.fadeRate = 0.0f;
}

void Scene::setAnimationWeight(AnimationHandle handle, float weight) {
    if (!handle.valid() || handle.index >= impl_->animations.size()) return;
    auto& animation = impl_->animations[handle.index];
    animation.weight = std::max(weight, 0.0f);
    animation.targetWeight = animation.weight;
    animation.fadeRate = 0.0f;
}

void Scene::crossfadeAnimation(AnimationHandle to, float fadeSeconds, bool loop, float speed) {
    if (!to.valid() || to.index >= impl_->animations.size()) return;
    const float rate = fadeSeconds > 0.0f ? 1.0f / fadeSeconds : 0.0f;
    for (uint32_t i = 0; i < impl_->animations.size(); ++i) {
        auto& animation = impl_->animations[i];
        if (i == to.index || !animation.playing) continue;
        animation.targetWeight = 0.0f;
        animation.fadeRate = rate;
    }
    auto& target = impl_->animations[to.index];
    if (!target.playing) {
        target.playing = true;
        target.time = 0.0;
        target.weight = fadeSeconds > 0.0f ? 0.0f : 1.0f;
    }
    target.loop = loop;
    target.speed = speed;
    target.targetWeight = 1.0f;
    target.fadeRate = rate;
}

void Scene::crossfadeAnimation(std::string_view name, float fadeSeconds, bool loop,
                               float speed) {
    const AnimationHandle handle = findAnimation(name);
    if (!handle.valid()) {
        log::warn("animation '{}' not found", name);
        return;
    }
    crossfadeAnimation(handle, fadeSeconds, loop, speed);
}

void Scene::playAnimation(std::string_view name, bool loop, float speed) {
    const AnimationHandle handle = findAnimation(name);
    if (!handle.valid()) {
        log::warn("animation '{}' not found", name);
        return;
    }
    playAnimation(handle, loop, speed);
}

void Scene::stopAnimation(AnimationHandle handle) {
    if (handle.valid() && handle.index < impl_->animations.size())
        impl_->animations[handle.index].playing = false;
}

void Scene::stopAllAnimations() {
    for (auto& animation : impl_->animations) animation.playing = false;
}

bool Scene::animationPlaying(AnimationHandle handle) const {
    return handle.valid() && handle.index < impl_->animations.size() &&
           impl_->animations[handle.index].playing;
}

void Scene::updateAnimations(float dt) {
    // Weighted blend: every playing clip's samples accumulate per node, then
    // resolve with normalized weights (so a crossfade sums to a full pose).
    auto& scratch = impl_->animationScratch;
    scratch.assign(impl_->nodes.size(), detail::TransformAccumulator{});
    bool anySamples = false;

    for (auto& animation : impl_->animations) {
        if (!animation.playing) continue;
        animation.time += static_cast<double>(dt * animation.speed);

        // Fades.
        if (animation.fadeRate > 0.0f)
            animation.weight = detail::advanceWeight(animation.weight, animation.targetWeight,
                                                     animation.fadeRate, dt);
        else
            animation.weight = animation.targetWeight;
        if (animation.weight <= 0.0f && animation.targetWeight <= 0.0f) {
            animation.playing = false; // faded out
            continue;
        }

        float sampleTime = 0.0f;
        const bool keepPlaying = detail::animationSampleTime(
            static_cast<float>(animation.time), animation.startTime, animation.duration,
            animation.loop, &sampleTime);
        if (!keepPlaying) animation.playing = false; // apply the final pose below

        for (const auto& channel : animation.channels) {
            if (channel.node >= scratch.size()) continue;
            if (channel.path == detail::AnimationChannel::Path::Weights) {
                thread_local std::vector<float> sampled;
                sampled.assign(channel.targetCount, 0.0f);
                detail::sampleAnimationWeights(channel, sampleTime, sampled.data());
                scratch[channel.node].addMorph(sampled.data(), channel.targetCount,
                                               animation.weight);
            } else {
                const Vec4 value = detail::sampleAnimationChannel(channel, sampleTime);
                scratch[channel.node].add(channel.path, value, animation.weight);
            }
            anySamples = true;
        }
    }

    if (!anySamples) return;
    for (uint32_t i = 0; i < scratch.size(); ++i) {
        scratch[i].apply(impl_->nodes[i].local, impl_->nodes[i].baseLocal);
        if (scratch[i].morphWeight > 0.0f) {
            scratch[i].applyMorph(impl_->nodes[i].morphWeights,
                                  impl_->nodes[i].baseMorphWeights);
            // Mirror onto mesh primitive children (they hold the meshes).
            for (uint32_t child = 0; child < impl_->nodes.size(); ++child)
                if (impl_->nodes[child].parent == i && impl_->nodes[child].mesh.valid())
                    impl_->nodes[child].morphWeights = impl_->nodes[i].morphWeights;
        }
    }
}

float Scene::approximateRadius(NodeId root) {
    if (!root.valid() || root.index >= impl_->nodes.size()) return 1.0f;
    impl_->updateWorldTransforms();
    const Vec3 origin = Vec3(impl_->nodes[root.index].world[3]);

    // Is `node` inside the subtree under root?
    auto inSubtree = [&](uint32_t index) {
        for (uint32_t cursor = index; cursor != UINT32_MAX;
             cursor = impl_->nodes[cursor].parent)
            if (cursor == root.index) return true;
        return false;
    };

    float radius = 0.0f;
    for (uint32_t i = 0; i < impl_->nodes.size(); ++i) {
        const detail::SceneNode& node = impl_->nodes[i];
        if (!node.alive || !node.mesh.valid() || !inSubtree(i)) continue;
        const detail::MeshRange& range = impl_->meshes->range(node.mesh);
        const Vec3 center = Vec3(node.world * Vec4{range.boundsCenter, 1.0f});
        const Vec3 scale{glm::length(Vec3(node.world[0])), glm::length(Vec3(node.world[1])),
                         glm::length(Vec3(node.world[2]))};
        const float worldRadius =
            range.boundsRadius * std::max(scale.x, std::max(scale.y, scale.z));
        radius = std::max(radius, glm::length(center - origin) + worldRadius);
    }
    return std::max(radius, 0.01f);
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
