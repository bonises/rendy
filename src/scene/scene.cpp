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

// Scene::loadGltf lives in gltf.cpp.

// --------------------------------------------------------------- animation

namespace {

// Sample one channel at time t (t already wrapped/clamped by the caller).
Vec4 sampleChannel(const detail::AnimationChannel& channel, float t) {
    using Interp = detail::AnimationChannel::Interpolation;
    const auto& times = channel.times;
    const bool cubic = channel.interpolation == Interp::CubicSpline;
    const size_t stride = cubic ? 3 : 1; // inTangent, value, outTangent
    auto valueAt = [&](size_t key) { return channel.values[key * stride + (cubic ? 1 : 0)]; };

    if (times.empty()) return Vec4{0.0f};
    if (t <= times.front()) return valueAt(0);
    if (t >= times.back()) return valueAt(times.size() - 1);

    const auto it = std::upper_bound(times.begin(), times.end(), t);
    const size_t next = static_cast<size_t>(it - times.begin());
    const size_t prev = next - 1;
    const float span = times[next] - times[prev];
    const float u = span > 0.0f ? (t - times[prev]) / span : 0.0f;

    switch (channel.interpolation) {
    case Interp::Step:
        return valueAt(prev);
    case Interp::Linear:
        if (channel.path == detail::AnimationChannel::Path::Rotation) {
            const Vec4 a = valueAt(prev);
            const Vec4 b = valueAt(next);
            const Quat qa{a.w, a.x, a.y, a.z};
            const Quat qb{b.w, b.x, b.y, b.z};
            const Quat q = glm::slerp(qa, qb, u);
            return {q.x, q.y, q.z, q.w};
        }
        return glm::mix(valueAt(prev), valueAt(next), u);
    case Interp::CubicSpline: {
        // glTF Hermite: p(u) = h00 v0 + h10 d b0 + h01 v1 + h11 d a1
        const Vec4 v0 = channel.values[prev * 3 + 1];
        const Vec4 b0 = channel.values[prev * 3 + 2]; // out-tangent of prev
        const Vec4 a1 = channel.values[next * 3 + 0]; // in-tangent of next
        const Vec4 v1 = channel.values[next * 3 + 1];
        const float u2 = u * u;
        const float u3 = u2 * u;
        Vec4 result = (2.0f * u3 - 3.0f * u2 + 1.0f) * v0 +
                      (u3 - 2.0f * u2 + u) * span * b0 +
                      (-2.0f * u3 + 3.0f * u2) * v1 + (u3 - u2) * span * a1;
        if (channel.path == detail::AnimationChannel::Path::Rotation) {
            const Quat q = glm::normalize(Quat{result.w, result.x, result.y, result.z});
            result = {q.x, q.y, q.z, q.w};
        }
        return result;
    }
    }
    return valueAt(prev);
}

} // namespace

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
    for (auto& animation : impl_->animations) {
        if (!animation.playing) continue;
        animation.time += static_cast<double>(dt * animation.speed);
        float t = static_cast<float>(animation.time);
        if (animation.duration > 0.0f) {
            if (animation.loop) {
                t = std::fmod(t, animation.duration);
                if (t < 0.0f) t += animation.duration;
            } else if (t >= animation.duration) {
                t = animation.duration;
                animation.playing = false;
            }
        }

        for (const auto& channel : animation.channels) {
            if (channel.node >= impl_->nodes.size()) continue;
            Transform& transform = impl_->nodes[channel.node].local;
            const Vec4 value = sampleChannel(channel, t);
            switch (channel.path) {
            case detail::AnimationChannel::Path::Translation:
                transform.position = Vec3(value);
                break;
            case detail::AnimationChannel::Path::Rotation:
                transform.rotation = Quat{value.w, value.x, value.y, value.z};
                break;
            case detail::AnimationChannel::Path::Scale:
                transform.scale = Vec3(value);
                break;
            }
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
