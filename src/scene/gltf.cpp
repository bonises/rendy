// glTF 2.0 import via fastgltf: meshes, PBR materials, textures (with the
// spec's sRGB/linear split), and the node hierarchy. Skinning/animation: v2.

#include "app/app_impl.hpp"
#include "rendy/scene/scene.hpp"
#include "scene/scene_impl.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>

#include <stb_image.h>

#include <cstring>
#include <filesystem>
#include <unordered_map>

namespace rendy {
namespace {

struct GltfLoader {
    detail::SceneImpl& scene;
    const fastgltf::Asset& asset;
    std::filesystem::path directory;

    // image index → TextureRef, separately for sRGB and linear use.
    std::unordered_map<size_t, TextureRef> srgbImages;
    std::unordered_map<size_t, TextureRef> linearImages;
    // glTF material index → rendy material id (0 = fallback default).
    std::vector<uint32_t> materialIds;
    // glTF mesh index → list of (mesh handle, material id) per primitive.
    std::vector<std::vector<std::pair<MeshHandle, uint32_t>>> meshPrimitives;

    TextureRef loadImage(size_t imageIndex, bool srgb) {
        auto& cache = srgb ? srgbImages : linearImages;
        if (auto it = cache.find(imageIndex); it != cache.end()) return it->second;

        const fastgltf::Image& image = asset.images[imageIndex];
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = nullptr;

        std::visit(
            fastgltf::visitor{
                [&](const fastgltf::sources::Array& bytes) {
                    pixels = stbi_load_from_memory(
                        reinterpret_cast<const stbi_uc*>(bytes.bytes.data()),
                        static_cast<int>(bytes.bytes.size()), &width, &height, &channels,
                        STBI_rgb_alpha);
                },
                [&](const fastgltf::sources::BufferView& view) {
                    const auto& bufferView = asset.bufferViews[view.bufferViewIndex];
                    const auto& buffer = asset.buffers[bufferView.bufferIndex];
                    if (const auto* array =
                            std::get_if<fastgltf::sources::Array>(&buffer.data)) {
                        pixels = stbi_load_from_memory(
                            reinterpret_cast<const stbi_uc*>(array->bytes.data() +
                                                             bufferView.byteOffset),
                            static_cast<int>(bufferView.byteLength), &width, &height,
                            &channels, STBI_rgb_alpha);
                    }
                },
                [&](const fastgltf::sources::URI& uri) {
                    const auto path = directory / std::filesystem::path(uri.uri.fspath());
                    pixels = stbi_load(path.string().c_str(), &width, &height, &channels,
                                       STBI_rgb_alpha);
                },
                [](const auto&) {},
            },
            image.data);

        TextureRef ref{};
        if (pixels != nullptr) {
            TextureOptions options;
            options.srgb = srgb;
            options.mipmaps = true;
            options.wrap = TextureOptions::Wrap::Repeat;
            auto created =
                scene.app->textures->createFromPixels(pixels, {width, height}, options);
            if (created) ref = created.value();
            stbi_image_free(pixels);
        } else {
            log::warn("gltf: failed to decode image #{}", imageIndex);
        }
        cache.emplace(imageIndex, ref);
        return ref;
    }

    TextureRef textureFor(const fastgltf::Optional<fastgltf::TextureInfo>& info, bool srgb) {
        if (!info.has_value()) return {};
        const auto& texture = asset.textures[info->textureIndex];
        if (!texture.imageIndex.has_value()) return {};
        return loadImage(*texture.imageIndex, srgb);
    }

    void loadMaterials() {
        materialIds.reserve(asset.materials.size());
        for (const fastgltf::Material& material : asset.materials) {
            detail::GpuMaterial gpuMaterial{};
            const auto& pbr = material.pbrData;
            // glTF factors are already linear.
            gpuMaterial.baseColorFactor = {pbr.baseColorFactor[0], pbr.baseColorFactor[1],
                                           pbr.baseColorFactor[2], pbr.baseColorFactor[3]};
            gpuMaterial.emissiveMetallic = {material.emissiveFactor[0] * material.emissiveStrength,
                                            material.emissiveFactor[1] * material.emissiveStrength,
                                            material.emissiveFactor[2] * material.emissiveStrength,
                                            pbr.metallicFactor};
            gpuMaterial.params = {pbr.roughnessFactor,
                                  material.normalTexture.has_value()
                                      ? material.normalTexture->scale
                                      : 1.0f,
                                  material.occlusionTexture.has_value()
                                      ? material.occlusionTexture->strength
                                      : 1.0f,
                                  0.0f};

            gpuMaterial.maps.x = textureFor(pbr.baseColorTexture, true).index;
            gpuMaterial.maps.y = textureFor(pbr.metallicRoughnessTexture, false).index;
            if (material.normalTexture.has_value()) {
                const auto& texture = asset.textures[material.normalTexture->textureIndex];
                if (texture.imageIndex.has_value())
                    gpuMaterial.maps.z = loadImage(*texture.imageIndex, false).index;
            }
            gpuMaterial.maps.w = textureFor(material.emissiveTexture, true).index;
            if (material.occlusionTexture.has_value()) {
                const auto& texture = asset.textures[material.occlusionTexture->textureIndex];
                if (texture.imageIndex.has_value())
                    gpuMaterial.maps2.x = loadImage(*texture.imageIndex, false).index;
            }

            scene.materials.push_back(gpuMaterial);
            materialIds.push_back(static_cast<uint32_t>(scene.materials.size() - 1));
        }
    }

    // Average tangents from triangle UV gradients — good enough until a
    // proper mikktspace pass.
    static void generateTangents(MeshData& mesh) {
        std::vector<Vec3> accumulated(mesh.vertices.size(), Vec3{0.0f});
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            Vertex& v0 = mesh.vertices[mesh.indices[i]];
            Vertex& v1 = mesh.vertices[mesh.indices[i + 1]];
            Vertex& v2 = mesh.vertices[mesh.indices[i + 2]];
            const Vec3 edge1 = v1.position - v0.position;
            const Vec3 edge2 = v2.position - v0.position;
            const Vec2 deltaUV1 = v1.uv - v0.uv;
            const Vec2 deltaUV2 = v2.uv - v0.uv;
            const float det = deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y;
            if (std::abs(det) < 1e-8f) continue;
            const Vec3 tangent = (edge1 * deltaUV2.y - edge2 * deltaUV1.y) / det;
            for (uint32_t index : {mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]})
                accumulated[index] += tangent;
        }
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            const Vec3 normal = mesh.vertices[i].normal;
            Vec3 tangent = accumulated[i] - normal * glm::dot(normal, accumulated[i]);
            if (glm::length(tangent) < 1e-6f)
                tangent = std::abs(normal.y) < 0.99f ? glm::cross(normal, Vec3{0, 1, 0})
                                                     : Vec3{1, 0, 0};
            mesh.vertices[i].tangent = Vec4{glm::normalize(tangent), 1.0f};
        }
    }

    void loadMeshes() {
        meshPrimitives.resize(asset.meshes.size());
        for (size_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex) {
            for (const fastgltf::Primitive& primitive : asset.meshes[meshIndex].primitives) {
                if (primitive.type != fastgltf::PrimitiveType::Triangles) continue;
                const auto* positionAttr = primitive.findAttribute("POSITION");
                if (positionAttr == primitive.attributes.end()) continue;

                MeshData mesh;
                const auto& positions = asset.accessors[positionAttr->accessorIndex];
                mesh.vertices.resize(positions.count);
                fastgltf::iterateAccessorWithIndex<Vec3>(
                    asset, positions,
                    [&](Vec3 value, size_t i) { mesh.vertices[i].position = value; });

                bool hasNormals = false;
                if (const auto* attr = primitive.findAttribute("NORMAL");
                    attr != primitive.attributes.end()) {
                    hasNormals = true;
                    fastgltf::iterateAccessorWithIndex<Vec3>(
                        asset, asset.accessors[attr->accessorIndex],
                        [&](Vec3 value, size_t i) { mesh.vertices[i].normal = value; });
                }
                if (const auto* attr = primitive.findAttribute("TEXCOORD_0");
                    attr != primitive.attributes.end()) {
                    fastgltf::iterateAccessorWithIndex<Vec2>(
                        asset, asset.accessors[attr->accessorIndex],
                        [&](Vec2 value, size_t i) { mesh.vertices[i].uv = value; });
                }
                bool hasTangents = false;
                if (const auto* attr = primitive.findAttribute("TANGENT");
                    attr != primitive.attributes.end()) {
                    hasTangents = true;
                    fastgltf::iterateAccessorWithIndex<Vec4>(
                        asset, asset.accessors[attr->accessorIndex],
                        [&](Vec4 value, size_t i) { mesh.vertices[i].tangent = value; });
                }

                if (primitive.indicesAccessor.has_value()) {
                    const auto& indices = asset.accessors[*primitive.indicesAccessor];
                    mesh.indices.resize(indices.count);
                    fastgltf::iterateAccessorWithIndex<uint32_t>(
                        asset, indices,
                        [&](uint32_t value, size_t i) { mesh.indices[i] = value; });
                }

                if (!hasNormals) {
                    // Flat-ish normals from triangle accumulation.
                    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                        Vertex& v0 = mesh.vertices[mesh.indices[i]];
                        Vertex& v1 = mesh.vertices[mesh.indices[i + 1]];
                        Vertex& v2 = mesh.vertices[mesh.indices[i + 2]];
                        const Vec3 normal = glm::normalize(glm::cross(
                            v1.position - v0.position, v2.position - v0.position));
                        v0.normal = v1.normal = v2.normal = normal;
                    }
                }
                if (!hasTangents) generateTangents(mesh);

                const uint32_t materialId =
                    primitive.materialIndex.has_value()
                        ? materialIds[*primitive.materialIndex]
                        : 0u;
                meshPrimitives[meshIndex].emplace_back(scene.meshes->add(mesh), materialId);
            }
        }
    }

    NodeId instantiate(Scene& publicScene, size_t nodeIndex, NodeId parent) {
        const fastgltf::Node& node = asset.nodes[nodeIndex];

        Transform transform;
        std::visit(fastgltf::visitor{
                       [&](const fastgltf::TRS& trs) {
                           transform.position = {trs.translation[0], trs.translation[1],
                                                 trs.translation[2]};
                           transform.rotation = Quat{trs.rotation[3], trs.rotation[0],
                                                     trs.rotation[1], trs.rotation[2]};
                           transform.scale = {trs.scale[0], trs.scale[1], trs.scale[2]};
                       },
                       [&](const fastgltf::math::fmat4x4& matrix) {
                           Mat4 m;
                           for (int col = 0; col < 4; ++col)
                               for (int row = 0; row < 4; ++row)
                                   m[col][row] = matrix[static_cast<size_t>(col)][static_cast<size_t>(row)];
                           transform.position = Vec3(m[3]);
                           transform.scale = {glm::length(Vec3(m[0])), glm::length(Vec3(m[1])),
                                              glm::length(Vec3(m[2]))};
                           const Mat3 rotation{Vec3(m[0]) / transform.scale.x,
                                               Vec3(m[1]) / transform.scale.y,
                                               Vec3(m[2]) / transform.scale.z};
                           transform.rotation = glm::quat_cast(rotation);
                       },
                   },
                   node.transform);

        const NodeId id = publicScene.addNode(transform, parent);
        if (node.meshIndex.has_value()) {
            for (const auto& [meshHandle, materialId] : meshPrimitives[*node.meshIndex]) {
                const NodeId primitiveNode =
                    publicScene.addMesh(meshHandle, MaterialHandle{materialId});
                publicScene.setParent(primitiveNode, id);
            }
        }
        for (size_t child : node.children) instantiate(publicScene, child, id);
        return id;
    }
};

} // namespace

Result<NodeId> Scene::loadGltf(const std::string& path) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None)
        return err("gltf: cannot open '{}': {}", path, fastgltf::getErrorMessage(data.error()));

    const auto directory = std::filesystem::path(path).parent_path();
    fastgltf::Parser parser;
    auto asset = parser.loadGltf(data.get(), directory,
                                 fastgltf::Options::LoadExternalBuffers |
                                     fastgltf::Options::LoadExternalImages |
                                     fastgltf::Options::GenerateMeshIndices);
    if (asset.error() != fastgltf::Error::None)
        return err("gltf: failed to parse '{}': {}", path,
                   fastgltf::getErrorMessage(asset.error()));

    GltfLoader loader{*impl_, asset.get(), directory, {}, {}, {}, {}};
    loader.loadMaterials();
    loader.loadMeshes();

    const NodeId root = addNode();
    const auto sceneIndex = asset->defaultScene.value_or(0);
    if (sceneIndex < asset->scenes.size())
        for (size_t nodeIndex : asset->scenes[sceneIndex].nodeIndices)
            loader.instantiate(*this, nodeIndex, root);

    log::info("gltf: loaded '{}' ({} meshes, {} materials, {} images)", path,
              asset->meshes.size(), asset->materials.size(), asset->images.size());
    return root;
}

} // namespace rendy
