// glTF 2.0 import via fastgltf: meshes, PBR materials, textures (with the
// spec's sRGB/linear split), the node hierarchy, skins and animations.
// Extensions: KHR_draco_mesh_compression (draco decoder) and
// KHR_texture_basisu (KTX2 → BC7 via the basis transcoder).

#include "app/app_impl.hpp"
#include "rendy/scene/scene.hpp"
#include "scene/scene_impl.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>

#include <basisu_transcoder.h>
#include <draco/compression/decode.h>

#include <stb_image.h>

#include <fmt/core.h>

#include <cstring>
#include <fstream>
#include <limits>
#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace rendy {
namespace {

constexpr uint8_t kKtx2Magic[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                    0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

bool isKtx2(const uint8_t* bytes, size_t size) {
    return size >= sizeof(kKtx2Magic) && std::memcmp(bytes, kKtx2Magic, sizeof(kKtx2Magic)) == 0;
}

std::once_flag basisInitFlag;

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
    // glTF node index → scene node index (filled during instantiate).
    std::vector<uint32_t> nodeMap;
    // Primitive scene nodes awaiting a skin (sceneNodeIndex, gltfSkinIndex).
    std::vector<std::pair<uint32_t, size_t>> pendingSkins;

    /// KTX2 (KHR_texture_basisu) with the full mip chain transcoded — to BC7
    /// where the device supports BC compression (all desktop GPUs), else to
    /// plain RGBA32 (BC7 is optional in Vulkan).
    TextureRef createFromKtx2(const uint8_t* bytes, size_t size, bool srgb) {
        std::call_once(basisInitFlag, [] { basist::basisu_transcoder_init(); });
        basist::ktx2_transcoder transcoder;
        if (!transcoder.init(bytes, static_cast<uint32_t>(size)) ||
            !transcoder.start_transcoding()) {
            log::warn("gltf: failed to parse KTX2 texture");
            return {};
        }
        const bool bc7 = scene.app->gpu->supportsBcTextures();
        if (!bc7)
            log::debug("gltf: no BC texture support — KTX2 transcodes to RGBA32");
        const auto target = bc7 ? basist::transcoder_texture_format::cTFBC7_RGBA
                                : basist::transcoder_texture_format::cTFRGBA32;
        std::vector<std::vector<uint8_t>> mipData;
        for (uint32_t level = 0; level < transcoder.get_levels(); ++level) {
            basist::ktx2_image_level_info info{};
            if (!transcoder.get_image_level_info(info, level, 0, 0)) return {};
            // The output unit is blocks for BC7 and pixels for RGBA32.
            const uint32_t units =
                bc7 ? info.m_total_blocks : info.m_orig_width * info.m_orig_height;
            std::vector<uint8_t> data(static_cast<size_t>(units) * (bc7 ? 16 : 4));
            if (!transcoder.transcode_image_level(level, 0, 0, data.data(), units, target)) {
                log::warn("gltf: KTX2 transcode failed at mip {}", level);
                return {};
            }
            mipData.push_back(std::move(data));
        }
        std::vector<gpu::TexturePool::CompressedMip> mips;
        mips.reserve(mipData.size());
        for (const auto& data : mipData) mips.push_back({data.data(), data.size()});
        TextureOptions options;
        options.srgb = srgb;
        options.mipmaps = mips.size() > 1;
        options.wrap = TextureOptions::Wrap::Repeat;
        const VkFormat format =
            bc7 ? (srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK)
                : (srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
        auto created = scene.app->textures->createCompressed(
            mips,
            {static_cast<int>(transcoder.get_width()), static_cast<int>(transcoder.get_height())},
            format, options);
        if (!created) log::warn("gltf: KTX2 upload failed: {}", created.error().message);
        return created ? created.value() : TextureRef{};
    }

    TextureRef createFromBytes(const uint8_t* bytes, size_t size, bool srgb) {
        if (isKtx2(bytes, size)) return createFromKtx2(bytes, size, srgb);
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(bytes, static_cast<int>(size), &width,
                                                &height, &channels, STBI_rgb_alpha);
        if (pixels == nullptr) return {};
        TextureOptions options;
        options.srgb = srgb;
        options.mipmaps = true;
        options.wrap = TextureOptions::Wrap::Repeat;
        auto created = scene.app->textures->createFromPixels(pixels, {width, height}, options);
        stbi_image_free(pixels);
        return created ? created.value() : TextureRef{};
    }

    TextureRef loadImage(size_t imageIndex, bool srgb) {
        auto& cache = srgb ? srgbImages : linearImages;
        if (auto it = cache.find(imageIndex); it != cache.end()) return it->second;

        const fastgltf::Image& image = asset.images[imageIndex];
        TextureRef ref{};
        std::visit(
            fastgltf::visitor{
                [&](const fastgltf::sources::Array& bytes) {
                    ref = createFromBytes(
                        reinterpret_cast<const uint8_t*>(bytes.bytes.data()),
                        bytes.bytes.size(), srgb);
                },
                [&](const fastgltf::sources::BufferView& view) {
                    const auto& bufferView = asset.bufferViews[view.bufferViewIndex];
                    const auto& buffer = asset.buffers[bufferView.bufferIndex];
                    if (const auto* array =
                            std::get_if<fastgltf::sources::Array>(&buffer.data)) {
                        ref = createFromBytes(
                            reinterpret_cast<const uint8_t*>(array->bytes.data()) +
                                bufferView.byteOffset,
                            bufferView.byteLength, srgb);
                    }
                },
                [&](const fastgltf::sources::URI& uri) {
                    const auto path = directory / std::filesystem::path(uri.uri.fspath());
                    std::ifstream file(path, std::ios::binary);
                    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                               std::istreambuf_iterator<char>()};
                    if (!bytes.empty()) ref = createFromBytes(bytes.data(), bytes.size(), srgb);
                },
                [](const auto&) {},
            },
            image.data);

        if (ref.index == 0) log::warn("gltf: failed to decode image #{}", imageIndex);
        cache.emplace(imageIndex, ref);
        return ref;
    }

    /// The image behind a texture — prefers the KHR_texture_basisu source.
    std::optional<size_t> imageOf(const fastgltf::Texture& texture) const {
        if (texture.basisuImageIndex.has_value()) return *texture.basisuImageIndex;
        if (texture.imageIndex.has_value()) return *texture.imageIndex;
        return std::nullopt;
    }

    TextureRef textureFor(const fastgltf::Optional<fastgltf::TextureInfo>& info, bool srgb) {
        if (!info.has_value()) return {};
        const auto image = imageOf(asset.textures[info->textureIndex]);
        if (!image.has_value()) return {};
        return loadImage(*image, srgb);
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
            const AlphaMode alphaMode =
                material.alphaMode == fastgltf::AlphaMode::Blend ? AlphaMode::Blend
                : material.alphaMode == fastgltf::AlphaMode::Mask ? AlphaMode::Mask
                                                                  : AlphaMode::Opaque;
            gpuMaterial.params = {pbr.roughnessFactor,
                                  material.normalTexture.has_value()
                                      ? material.normalTexture->scale
                                      : 1.0f,
                                  material.occlusionTexture.has_value()
                                      ? material.occlusionTexture->strength
                                      : 1.0f,
                                  alphaMode == AlphaMode::Mask ? material.alphaCutoff : 0.0f};

            gpuMaterial.maps.x = textureFor(pbr.baseColorTexture, true).index;
            gpuMaterial.maps.y = textureFor(pbr.metallicRoughnessTexture, false).index;
            if (material.normalTexture.has_value()) {
                const auto image =
                    imageOf(asset.textures[material.normalTexture->textureIndex]);
                if (image.has_value()) gpuMaterial.maps.z = loadImage(*image, false).index;
            }
            gpuMaterial.maps.w = textureFor(material.emissiveTexture, true).index;
            if (material.occlusionTexture.has_value()) {
                const auto image =
                    imageOf(asset.textures[material.occlusionTexture->textureIndex]);
                if (image.has_value()) gpuMaterial.maps2.x = loadImage(*image, false).index;
            }

            scene.materials.push_back(gpuMaterial);
            scene.materialAlphaModes.push_back(alphaMode);
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

    /// KHR_draco_mesh_compression: vertices/indices come from the draco
    /// blob, not the accessors (draco may reorder vertices).
    bool loadDracoPrimitive(const fastgltf::Primitive& primitive, MeshData& mesh,
                            bool& hasNormals, bool& hasTangents) {
        const auto& dracoExt = *primitive.dracoCompression;
        if (dracoExt.bufferView >= asset.bufferViews.size()) return false;
        const auto& bufferView = asset.bufferViews[dracoExt.bufferView];
        const auto* array =
            std::get_if<fastgltf::sources::Array>(&asset.buffers[bufferView.bufferIndex].data);
        if (array == nullptr) return false;

        draco::DecoderBuffer decoderBuffer;
        decoderBuffer.Init(
            reinterpret_cast<const char*>(array->bytes.data()) + bufferView.byteOffset,
            bufferView.byteLength);
        draco::Decoder decoder;
        auto decoded = decoder.DecodeMeshFromBuffer(&decoderBuffer);
        if (!decoded.ok()) {
            log::warn("gltf: draco decode failed: {}", decoded.status().error_msg());
            return false;
        }
        const draco::Mesh& dracoMesh = *decoded.value();

        const uint32_t vertexCount = dracoMesh.num_points();
        mesh.vertices.resize(vertexCount);
        mesh.indices.reserve(static_cast<size_t>(dracoMesh.num_faces()) * 3);
        for (draco::FaceIndex face(0); face < dracoMesh.num_faces(); ++face)
            for (int corner = 0; corner < 3; ++corner)
                mesh.indices.push_back(dracoMesh.face(face)[corner].value());

        // The extension maps attribute names to draco unique ids.
        auto attribute = [&](std::string_view name) -> const draco::PointAttribute* {
            const auto it = dracoExt.findAttribute(name);
            if (it == dracoExt.attributes.cend()) return nullptr;
            return dracoMesh.GetAttributeByUniqueId(static_cast<uint32_t>(it->accessorIndex));
        };
        auto readFloats = [&](const draco::PointAttribute* attr, int components, auto&& sink) {
            float value[4] = {0, 0, 0, 0};
            for (uint32_t i = 0; i < vertexCount; ++i) {
                attr->ConvertValue<float>(attr->mapped_index(draco::PointIndex(i)),
                                          static_cast<int8_t>(components), value);
                sink(i, value);
            }
        };

        const draco::PointAttribute* positions = attribute("POSITION");
        if (positions == nullptr) return false;
        readFloats(positions, 3, [&](uint32_t i, const float* v) {
            mesh.vertices[i].position = {v[0], v[1], v[2]};
        });
        if (const auto* attr = attribute("NORMAL")) {
            hasNormals = true;
            readFloats(attr, 3, [&](uint32_t i, const float* v) {
                mesh.vertices[i].normal = {v[0], v[1], v[2]};
            });
        }
        if (const auto* attr = attribute("TEXCOORD_0")) {
            readFloats(attr, 2,
                       [&](uint32_t i, const float* v) { mesh.vertices[i].uv = {v[0], v[1]}; });
        }
        if (const auto* attr = attribute("TANGENT")) {
            hasTangents = true;
            readFloats(attr, 4, [&](uint32_t i, const float* v) {
                mesh.vertices[i].tangent = {v[0], v[1], v[2], v[3]};
            });
        }
        if (const auto* attr = attribute("JOINTS_0")) {
            uint32_t joints[4] = {0, 0, 0, 0};
            for (uint32_t i = 0; i < vertexCount; ++i) {
                attr->ConvertValue<uint32_t>(attr->mapped_index(draco::PointIndex(i)), 4,
                                             joints);
                mesh.vertices[i].joints = {static_cast<uint16_t>(joints[0]),
                                           static_cast<uint16_t>(joints[1]),
                                           static_cast<uint16_t>(joints[2]),
                                           static_cast<uint16_t>(joints[3])};
            }
        }
        if (const auto* attr = attribute("WEIGHTS_0")) {
            readFloats(attr, 4, [&](uint32_t i, const float* v) {
                const Vec4 value{v[0], v[1], v[2], v[3]};
                const float sum = value.x + value.y + value.z + value.w;
                mesh.vertices[i].weights = sum > 0.0f ? value / sum : value;
            });
        }
        return true;
    }

    void loadMeshes() {
        meshPrimitives.resize(asset.meshes.size());
        for (size_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex) {
            for (const fastgltf::Primitive& primitive : asset.meshes[meshIndex].primitives) {
                if (primitive.type != fastgltf::PrimitiveType::Triangles) continue;

                MeshData mesh;
                bool hasNormals = false;
                bool hasTangents = false;
                if (primitive.dracoCompression != nullptr) {
                    if (!loadDracoPrimitive(primitive, mesh, hasNormals, hasTangents))
                        continue;
                    if (!primitive.targets.empty()) {
                        // Draco reorders vertices; target accessors keep the
                        // original order, so the deltas would land on the
                        // wrong vertices.
                        log::warn("gltf: morph targets on a draco-compressed primitive are "
                                  "not supported — skipping the targets");
                    }
                } else {
                    const auto* positionAttr = primitive.findAttribute("POSITION");
                    if (positionAttr == primitive.attributes.end()) continue;
                    const auto& positions = asset.accessors[positionAttr->accessorIndex];
                    mesh.vertices.resize(positions.count);
                    fastgltf::iterateAccessorWithIndex<Vec3>(
                        asset, positions,
                        [&](Vec3 value, size_t i) { mesh.vertices[i].position = value; });

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
                    if (const auto* attr = primitive.findAttribute("JOINTS_0");
                        attr != primitive.attributes.end()) {
                        fastgltf::iterateAccessorWithIndex<glm::u16vec4>(
                            asset, asset.accessors[attr->accessorIndex],
                            [&](glm::u16vec4 value, size_t i) {
                                mesh.vertices[i].joints = value;
                            });
                    }
                    if (const auto* attr = primitive.findAttribute("WEIGHTS_0");
                        attr != primitive.attributes.end()) {
                        fastgltf::iterateAccessorWithIndex<Vec4>(
                            asset, asset.accessors[attr->accessorIndex],
                            [&](Vec4 value, size_t i) {
                                const float sum = value.x + value.y + value.z + value.w;
                                mesh.vertices[i].weights = sum > 0.0f ? value / sum : value;
                            });
                    }
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
                }

                // Morph targets (POSITION/NORMAL deltas). Not for draco
                // primitives — see the warning above.
                const size_t targetCount =
                    primitive.dracoCompression == nullptr ? primitive.targets.size() : 0;
                for (size_t targetIndex = 0; targetIndex < targetCount; ++targetIndex) {
                    MorphTarget target;
                    if (auto attr = primitive.findTargetAttribute(targetIndex, "POSITION");
                        attr != primitive.targets[targetIndex].end()) {
                        target.positionDeltas.resize(mesh.vertices.size(), Vec3{0.0f});
                        fastgltf::iterateAccessorWithIndex<Vec3>(
                            asset, asset.accessors[attr->accessorIndex],
                            [&](Vec3 value, size_t i) { target.positionDeltas[i] = value; });
                    }
                    if (auto attr = primitive.findTargetAttribute(targetIndex, "NORMAL");
                        attr != primitive.targets[targetIndex].end()) {
                        target.normalDeltas.resize(mesh.vertices.size(), Vec3{0.0f});
                        fastgltf::iterateAccessorWithIndex<Vec3>(
                            asset, asset.accessors[attr->accessorIndex],
                            [&](Vec3 value, size_t i) { target.normalDeltas[i] = value; });
                    }
                    mesh.morphTargets.push_back(std::move(target));
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
        if (nodeIndex < nodeMap.size()) nodeMap[nodeIndex] = id.index;
        if (node.meshIndex.has_value()) {
            std::vector<float> initialWeights;
            const auto& weightSource =
                !node.weights.empty() ? node.weights : asset.meshes[*node.meshIndex].weights;
            initialWeights.assign(weightSource.begin(), weightSource.end());
            for (const auto& [meshHandle, materialId] : meshPrimitives[*node.meshIndex]) {
                const NodeId primitiveNode =
                    publicScene.addMesh(meshHandle, MaterialHandle{materialId});
                publicScene.setParent(primitiveNode, id);
                if (node.skinIndex.has_value())
                    pendingSkins.emplace_back(primitiveNode.index, *node.skinIndex);
                if (!initialWeights.empty()) {
                    scene.nodes[primitiveNode.index].morphWeights = initialWeights;
                    scene.nodes[primitiveNode.index].baseMorphWeights = initialWeights;
                    scene.nodes[id.index].morphWeights = initialWeights;
                    scene.nodes[id.index].baseMorphWeights = initialWeights;
                }
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
    fastgltf::Parser parser(fastgltf::Extensions::KHR_draco_mesh_compression |
                            fastgltf::Extensions::KHR_texture_basisu |
                            fastgltf::Extensions::KHR_mesh_quantization |
                            fastgltf::Extensions::KHR_materials_emissive_strength);
    auto asset = parser.loadGltf(data.get(), directory,
                                 fastgltf::Options::LoadExternalBuffers |
                                     fastgltf::Options::LoadExternalImages |
                                     fastgltf::Options::GenerateMeshIndices);
    if (asset.error() != fastgltf::Error::None)
        return err("gltf: failed to parse '{}': {}", path,
                   fastgltf::getErrorMessage(asset.error()));

    GltfLoader loader{*impl_, asset.get(), directory, {}, {}, {}, {}, {}, {}};
    loader.nodeMap.assign(asset->nodes.size(), UINT32_MAX);
    loader.loadMaterials();
    loader.loadMeshes();

    const NodeId root = addNode();
    const auto sceneIndex = asset->defaultScene.value_or(0);
    if (sceneIndex < asset->scenes.size())
        for (size_t nodeIndex : asset->scenes[sceneIndex].nodeIndices)
            loader.instantiate(*this, nodeIndex, root);

    // ---- skins (joints reference glTF nodes → resolve via nodeMap)
    const size_t skinBase = impl_->skins.size();
    for (const fastgltf::Skin& skin : asset->skins) {
        detail::Skin sceneSkin;
        sceneSkin.jointNodes.reserve(skin.joints.size());
        for (size_t joint : skin.joints)
            sceneSkin.jointNodes.push_back(joint < loader.nodeMap.size()
                                               ? loader.nodeMap[joint]
                                               : UINT32_MAX);
        sceneSkin.inverseBind.assign(skin.joints.size(), Mat4{1.0f});
        if (skin.inverseBindMatrices.has_value()) {
            fastgltf::iterateAccessorWithIndex<Mat4>(
                asset.get(), asset->accessors[*skin.inverseBindMatrices],
                [&](const Mat4& matrix, size_t i) { sceneSkin.inverseBind[i] = matrix; });
        }
        impl_->skins.push_back(std::move(sceneSkin));
    }
    for (const auto& [sceneNode, gltfSkin] : loader.pendingSkins)
        impl_->nodes[sceneNode].skinIndex = static_cast<int32_t>(skinBase + gltfSkin);

    // ---- animations
    for (const fastgltf::Animation& animation : asset->animations) {
        detail::SceneAnimation clip;
        clip.name = animation.name.empty()
                        ? fmt::format("animation_{}", impl_->animations.size())
                        : std::string(animation.name);
        for (const fastgltf::AnimationChannel& channel : animation.channels) {
            if (!channel.nodeIndex.has_value()) continue;
            const bool isWeights = channel.path == fastgltf::AnimationPath::Weights;
            if (!isWeights && channel.path != fastgltf::AnimationPath::Translation &&
                channel.path != fastgltf::AnimationPath::Rotation &&
                channel.path != fastgltf::AnimationPath::Scale)
                continue;
            const uint32_t sceneNode = *channel.nodeIndex < loader.nodeMap.size()
                                           ? loader.nodeMap[*channel.nodeIndex]
                                           : UINT32_MAX;
            if (sceneNode == UINT32_MAX) continue;

            const fastgltf::AnimationSampler& sampler = animation.samplers[channel.samplerIndex];
            detail::AnimationChannel sceneChannel;
            sceneChannel.node = sceneNode;
            sceneChannel.path =
                isWeights ? detail::AnimationChannel::Path::Weights
                : channel.path == fastgltf::AnimationPath::Translation
                    ? detail::AnimationChannel::Path::Translation
                    : channel.path == fastgltf::AnimationPath::Rotation
                          ? detail::AnimationChannel::Path::Rotation
                          : detail::AnimationChannel::Path::Scale;
            sceneChannel.interpolation =
                sampler.interpolation == fastgltf::AnimationInterpolation::Step
                    ? detail::AnimationChannel::Interpolation::Step
                    : sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline
                          ? detail::AnimationChannel::Interpolation::CubicSpline
                          : detail::AnimationChannel::Interpolation::Linear;

            const auto& input = asset->accessors[sampler.inputAccessor];
            sceneChannel.times.resize(input.count);
            fastgltf::iterateAccessorWithIndex<float>(
                asset.get(), input, [&](float t, size_t i) { sceneChannel.times[i] = t; });

            const auto& output = asset->accessors[sampler.outputAccessor];
            if (isWeights) {
                // Scalars: keyCount * targetCount (cubic: ×3).
                const fastgltf::Node& gltfNode = asset->nodes[*channel.nodeIndex];
                if (gltfNode.meshIndex.has_value())
                    sceneChannel.targetCount = static_cast<uint32_t>(
                        asset->meshes[*gltfNode.meshIndex].primitives.empty()
                            ? 0
                            : asset->meshes[*gltfNode.meshIndex]
                                  .primitives[0]
                                  .targets.size());
                sceneChannel.scalarValues.resize(output.count, 0.0f);
                fastgltf::iterateAccessorWithIndex<float>(
                    asset.get(), output,
                    [&](float value, size_t i) { sceneChannel.scalarValues[i] = value; });
                clip.channels.push_back(std::move(sceneChannel));
                continue;
            }
            sceneChannel.values.resize(output.count, Vec4{0.0f});
            if (sceneChannel.path == detail::AnimationChannel::Path::Rotation) {
                fastgltf::iterateAccessorWithIndex<Vec4>(
                    asset.get(), output,
                    [&](Vec4 value, size_t i) { sceneChannel.values[i] = value; });
            } else {
                fastgltf::iterateAccessorWithIndex<Vec3>(
                    asset.get(), output, [&](Vec3 value, size_t i) {
                        sceneChannel.values[i] = Vec4{value, 0.0f};
                    });
            }
            clip.channels.push_back(std::move(sceneChannel));
        }
        // Clips need not start at t=0 (trimmed exports); track the window.
        float clipStart = std::numeric_limits<float>::max();
        float clipEnd = 0.0f;
        for (const auto& channel : clip.channels) {
            if (channel.times.empty()) continue;
            clipStart = std::min(clipStart, channel.times.front());
            clipEnd = std::max(clipEnd, channel.times.back());
        }
        if (clipStart <= clipEnd) {
            clip.startTime = clipStart;
            clip.duration = clipEnd - clipStart;
        }
        impl_->animations.push_back(std::move(clip));
    }

    log::info("gltf: loaded '{}' ({} meshes, {} materials, {} images, {} skins, {} animations)",
              path, asset->meshes.size(), asset->materials.size(), asset->images.size(),
              asset->skins.size(), asset->animations.size());
    return root;
}

} // namespace rendy
