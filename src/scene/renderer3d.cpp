#include "scene/renderer3d.hpp"

#include "scene/env_baker.hpp"

#include "shaders/env_bake_vert_spv.h"
#include "shaders/env_brdf_lut_frag_spv.h"
#include "shaders/env_prefilter_frag_spv.h"
#include "shaders/mesh_frag_spv.h"
#include "shaders/mesh_vert_spv.h"
#include "shaders/shadow_vert_spv.h"
#include "shaders/skybox_frag_spv.h"
#include "shaders/skybox_vert_spv.h"
#include "shaders/tonemap_frag_spv.h"
#include "shaders/tonemap_vert_spv.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>

namespace rendy::detail {
namespace {

// Matches shaders/scene_common.glsl FrameData.
struct FrameUbo {
    Mat4 view;
    Mat4 proj;
    Mat4 viewProj;
    Mat4 invViewProj;
    Mat4 cascadeMatrices[Renderer3D::kMaxCascades];
    Mat4 spotMatrices[Renderer3D::kMaxSpotShadows];
    Vec4 cascadeSplits;
    Vec4 viewPos;
    Vec4 ambient;
    glm::uvec4 counts;
    Vec4 pointShadowParams;
    Vec4 clusterParams; // tileW, tileH, sliceScale, sliceBias
    Vec4 probePositions[Renderer3D::kMaxProbes]; // xyz pos, w = active
    Vec4 probeBoxMins[Renderer3D::kMaxProbes];   // xyz min, w = fade
    Vec4 probeBoxMaxs[Renderer3D::kMaxProbes];   // xyz max
};

constexpr uint32_t kNoJoints = 0xFFFFFFFFu;

struct MeshPush {
    uint32_t transformIndex;
    uint32_t materialIndex;
    uint32_t jointBase; // kNoJoints = unskinned
    uint32_t morphWeightBase;
    uint32_t morphDeltaBase;
    uint32_t morphTargetCount; // 0 = no morphs
    uint32_t meshVertexBase;
    uint32_t meshVertexCount;
};

struct ShadowPush {
    Mat4 lightViewProj;
    uint32_t transformIndex;
    uint32_t jointBase;
    uint32_t morphWeightBase;
    uint32_t morphDeltaBase;
    uint32_t morphTargetCount;
    uint32_t meshVertexBase;
    uint32_t meshVertexCount;
    uint32_t pad;
};

constexpr float kPointShadowNear = 0.05f;

struct TonemapPush {
    uint32_t hdrTexture;
    uint32_t tonemapper;
    float exposure;
};

VkShaderModule createModule(VkDevice device, const uint32_t* code, size_t words) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = words * sizeof(uint32_t);
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

struct FrustumPlanes {
    Vec4 planes[6];

    explicit FrustumPlanes(const Mat4& viewProj) {
        const Mat4 m = glm::transpose(viewProj);
        planes[0] = m[3] + m[0]; // left
        planes[1] = m[3] - m[0]; // right
        planes[2] = m[3] + m[1]; // bottom
        planes[3] = m[3] - m[1]; // top
        planes[4] = m[2];        // near (Vulkan/D3D clip: 0 <= z)
        planes[5] = m[3] - m[2]; // far (z <= w)
        for (Vec4& plane : planes) {
            const float length = glm::length(Vec3(plane));
            if (length > 0.0f) plane /= length;
        }
    }

    [[nodiscard]] bool visible(Vec3 center, float radius) const {
        for (const Vec4& plane : planes)
            if (glm::dot(Vec3(plane), center) + plane.w < -radius) return false;
        return true;
    }
};

} // namespace

Renderer3D::Renderer3D(gpu::Context& ctx, gpu::BindlessTable& bindless,
                       gpu::Uploader& uploader, VkFormat swapchainFormat)
    : ctx_(ctx), bindless_(bindless) {
    // Set 1: UBO + transforms/materials/lights/joints + 3 shadow map arrays
    // + environment cubes (8,9,10) + BRDF LUT (11) + morph deltas/weights
    // + forward+ clusters (14,15) + reflection probe cube array (16).
    VkDescriptorSetLayoutBinding bindings[17]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (uint32_t i = 1; i < 4; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (uint32_t i = 4; i < 7; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (uint32_t i = 8; i < 12; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (uint32_t i = 12; i < 14; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                       nullptr};
    for (uint32_t i = 14; i < 16; ++i) // forward+ cluster data
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                       nullptr};
    bindings[16] = {16, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 17;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx_.device(), &layoutInfo, nullptr, &frameSetLayout_));

    // One set per frame in flight + one for the blocking probe bake.
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, gpu::kFramesInFlight + 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8 * (gpu::kFramesInFlight + 1)},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 * (gpu::kFramesInFlight + 1)},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = gpu::kFramesInFlight + 1;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx_.device(), &poolInfo, nullptr, &descriptorPool_));

    VkDescriptorSetLayout layouts[gpu::kFramesInFlight + 1];
    for (auto& layout : layouts) layout = frameSetLayout_;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = gpu::kFramesInFlight;
    allocInfo.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(ctx_.device(), &allocInfo, frameSets_));
    allocInfo.descriptorSetCount = 1;
    VK_CHECK(vkAllocateDescriptorSets(ctx_.device(), &allocInfo, &bakeSet_));

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx_.device(), &samplerInfo, nullptr, &resolveSampler_));

    // Shadow samplers: hardware-compare PCF sampler + a plain one for the
    // manually compared point cube maps.
    VkSamplerCreateInfo shadowSamplerInfo = samplerInfo;
    shadowSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadowSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadowSamplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    shadowSamplerInfo.compareEnable = VK_TRUE;
    shadowSamplerInfo.compareOp = VK_COMPARE_OP_LESS;
    VK_CHECK(vkCreateSampler(ctx_.device(), &shadowSamplerInfo, nullptr, &shadowSampler_));
    VK_CHECK(vkCreateSampler(ctx_.device(), &samplerInfo, nullptr, &pointShadowSampler_));

    createShadowArray(&cascadeShadows_, 2048, kMaxCascades, false);
    createShadowArray(&spotShadows_, 1024, kMaxSpotShadows, false);
    createShadowArray(&pointShadows_, 512, kMaxPointShadows * 6, true);

    // Default environment: 1x1 black cube + LUT keep bindings 8-11 valid
    // when no environment is set.
    {
        VkImageCreateInfo cubeInfo{};
        cubeInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        cubeInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        cubeInfo.imageType = VK_IMAGE_TYPE_2D;
        cubeInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        cubeInfo.extent = {1, 1, 1};
        cubeInfo.mipLevels = 1;
        cubeInfo.arrayLayers = 6;
        cubeInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        cubeInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocCreate{};
        allocCreate.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VK_CHECK(vmaCreateImage(ctx_.allocator(), &cubeInfo, &allocCreate, &defaultEnv_.cube,
                                &defaultEnv_.cubeAllocation, nullptr));
        // The BRDF LUT is environment-independent — bake a real one up front
        // so reflection probes have correct specular even without an HDRI.
        VkImageCreateInfo lutInfo = cubeInfo;
        lutInfo.flags = 0;
        lutInfo.format = VK_FORMAT_R16G16_SFLOAT;
        lutInfo.extent = {kBrdfLutSize, kBrdfLutSize, 1};
        lutInfo.arrayLayers = 1;
        lutInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VK_CHECK(vmaCreateImage(ctx_.allocator(), &lutInfo, &allocCreate, &defaultEnv_.lut,
                                &defaultEnv_.lutAllocation, nullptr));

        VkImageCreateInfo probeInfo = cubeInfo;
        probeInfo.extent = {kProbeSize, kProbeSize, 1};
        probeInfo.mipLevels = kProbeMips;
        probeInfo.arrayLayers = kMaxProbes * 6;
        probeInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VK_CHECK(vmaCreateImage(ctx_.allocator(), &probeInfo, &allocCreate, &probeArray_.image,
                                &probeArray_.allocation, nullptr));

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = defaultEnv_.cube;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        VK_CHECK(vkCreateImageView(ctx_.device(), &viewInfo, nullptr, &defaultEnv_.cubeView));
        viewInfo.image = defaultEnv_.lut;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx_.device(), &viewInfo, nullptr, &defaultEnv_.lutView));
        viewInfo.image = probeArray_.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kProbeMips, 0,
                                     kMaxProbes * 6};
        VK_CHECK(vkCreateImageView(ctx_.device(), &viewInfo, nullptr, &probeArray_.arrayView));

        VkSamplerCreateInfo defSampler{};
        defSampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        defSampler.magFilter = VK_FILTER_LINEAR;
        defSampler.minFilter = VK_FILTER_LINEAR;
        defSampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        defSampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        defSampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        defSampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        defSampler.maxLod = VK_LOD_CLAMP_NONE;
        VK_CHECK(vkCreateSampler(ctx_.device(), &defSampler, nullptr, &defaultEnv_.sampler));

        // Clear cube + probe array to black and move to SHADER_READ.
        const uint8_t zero = 0;
        uploader.submit(&zero, 1, [&](VkCommandBuffer cmd, VkBuffer) {
            for (VkImage image : {defaultEnv_.cube, probeArray_.image}) {
                gpu::imageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                                  VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
                const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                                    VK_REMAINING_MIP_LEVELS, 0,
                                                    VK_REMAINING_ARRAY_LAYERS};
                vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black,
                                     1, &range);
                gpu::imageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
        });

        // Bake the split-sum BRDF LUT (one fullscreen pass, blocking).
        EnvBaker lutBaker(ctx_, env_bake_vert_spv, env_bake_vert_spv_words);
        VkPipeline lutPipeline = lutBaker.makePipeline(
            env_brdf_lut_frag_spv, env_brdf_lut_frag_spv_words, VK_FORMAT_R16G16_SFLOAT);
        lutBaker.begin();
        envWholeImageBarrier(lutBaker.cmd, defaultEnv_.lut, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        lutBaker.renderFace(lutPipeline, VK_NULL_HANDLE, defaultEnv_.lutView, kBrdfLutSize, 0,
                            0.0f);
        envWholeImageBarrier(lutBaker.cmd, defaultEnv_.lut,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        lutBaker.submitAndWait();
    }

    swapchainFormat_ = swapchainFormat;
    meshVertBlob_ = gpu::ShaderBlob(mesh_vert_spv, mesh_vert_spv_words);
    meshFragBlob_ = gpu::ShaderBlob(mesh_frag_spv, mesh_frag_spv_words);
    tonemapVertBlob_ = gpu::ShaderBlob(tonemap_vert_spv, tonemap_vert_spv_words);
    tonemapFragBlob_ = gpu::ShaderBlob(tonemap_frag_spv, tonemap_frag_spv_words);
    shadowVertBlob_ = gpu::ShaderBlob(shadow_vert_spv, shadow_vert_spv_words);
    skyboxVertBlob_ = gpu::ShaderBlob(skybox_vert_spv, skybox_vert_spv_words);
    skyboxFragBlob_ = gpu::ShaderBlob(skybox_frag_spv, skybox_frag_spv_words);
    createLayouts();
    createPipelines();
}

void Renderer3D::createLayouts() {
    // ---- mesh/shadow pipeline layout (push constants sized for ShadowPush)
    VkPushConstantRange meshPush{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(ShadowPush)};
    VkDescriptorSetLayout meshSets[] = {bindless_.layout(), frameSetLayout_};
    VkPipelineLayoutCreateInfo meshLayoutInfo{};
    meshLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    meshLayoutInfo.setLayoutCount = 2;
    meshLayoutInfo.pSetLayouts = meshSets;
    meshLayoutInfo.pushConstantRangeCount = 1;
    meshLayoutInfo.pPushConstantRanges = &meshPush;
    VK_CHECK(vkCreatePipelineLayout(ctx_.device(), &meshLayoutInfo, nullptr, &meshLayout_));

    // ---- tonemap pipeline layout
    VkPushConstantRange tonemapPush{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TonemapPush)};
    VkDescriptorSetLayout tonemapSets[] = {bindless_.layout()};
    VkPipelineLayoutCreateInfo tonemapLayoutInfo{};
    tonemapLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    tonemapLayoutInfo.setLayoutCount = 1;
    tonemapLayoutInfo.pSetLayouts = tonemapSets;
    tonemapLayoutInfo.pushConstantRangeCount = 1;
    tonemapLayoutInfo.pPushConstantRanges = &tonemapPush;
    VK_CHECK(
        vkCreatePipelineLayout(ctx_.device(), &tonemapLayoutInfo, nullptr, &tonemapLayout_));
}

void Renderer3D::createShadowArray(ShadowArray* array, uint32_t size, uint32_t layers,
                                   bool cube) {
    array->size = size;
    array->layers = layers;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    if (cube) imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kDepthFormat;
    imageInfo.extent = {size, size, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = layers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(ctx_.allocator(), &imageInfo, &allocCreate, &array->image,
                            &array->allocation, nullptr));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = array->image;
    viewInfo.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = kDepthFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    VK_CHECK(vkCreateImageView(ctx_.device(), &viewInfo, nullptr, &array->sampleView));

    array->layerViews.resize(layers);
    for (uint32_t layer = 0; layer < layers; ++layer) {
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1};
        VK_CHECK(vkCreateImageView(ctx_.device(), &viewInfo, nullptr,
                                   &array->layerViews[layer]));
    }
}

bool Renderer3D::reloadShader(std::string_view name, std::vector<uint32_t> spirv,
                              gpu::FrameRing& frames) {
    gpu::ShaderBlob* blob = nullptr;
    if (name == "mesh.vert") blob = &meshVertBlob_;
    else if (name == "mesh.frag") blob = &meshFragBlob_;
    else if (name == "tonemap.vert") blob = &tonemapVertBlob_;
    else if (name == "tonemap.frag") blob = &tonemapFragBlob_;
    else if (name == "shadow.vert") blob = &shadowVertBlob_;
    else if (name == "skybox.vert") blob = &skyboxVertBlob_;
    else if (name == "skybox.frag") blob = &skyboxFragBlob_;
    else return false;
    blob->replace(std::move(spirv));

    // Rebuild everything from the current blobs; cheap with the pipeline
    // cache, and keeps the reload path to a single code path.
    VkDevice device = ctx_.device();
    for (VkPipeline pipeline : {meshPipeline_, meshBlendPipeline_, tonemapPipeline_,
                                shadowPipeline_, skyboxPipeline_, meshProbePipeline_,
                                skyboxProbePipeline_}) {
        frames.defer([device, pipeline] { vkDestroyPipeline(device, pipeline, nullptr); });
    }
    createPipelines();
    return true;
}

void Renderer3D::destroyShadowArray(ShadowArray* array) {
    for (VkImageView view : array->layerViews)
        vkDestroyImageView(ctx_.device(), view, nullptr);
    array->layerViews.clear();
    if (array->sampleView) vkDestroyImageView(ctx_.device(), array->sampleView, nullptr);
    if (array->image) vmaDestroyImage(ctx_.allocator(), array->image, array->allocation);
    *array = {};
}

Renderer3D::~Renderer3D() {
    destroyTargets();
    destroyShadowArray(&cascadeShadows_);
    destroyShadowArray(&spotShadows_);
    destroyShadowArray(&pointShadows_);
    vkDestroySampler(ctx_.device(), shadowSampler_, nullptr);
    vkDestroySampler(ctx_.device(), pointShadowSampler_, nullptr);
    vkDestroyPipeline(ctx_.device(), shadowPipeline_, nullptr);
    for (auto& buffer : uboBuffers_)
        if (buffer.buffer) vmaDestroyBuffer(ctx_.allocator(), buffer.buffer, buffer.allocation);
    for (auto& buffer : transformBuffers_)
        if (buffer.buffer) vmaDestroyBuffer(ctx_.allocator(), buffer.buffer, buffer.allocation);
    for (auto& buffer : materialBuffers_)
        if (buffer.buffer) vmaDestroyBuffer(ctx_.allocator(), buffer.buffer, buffer.allocation);
    for (auto& buffer : lightBuffers_)
        if (buffer.buffer) vmaDestroyBuffer(ctx_.allocator(), buffer.buffer, buffer.allocation);
    for (auto& buffer : jointBuffers_)
        if (buffer.buffer) vmaDestroyBuffer(ctx_.allocator(), buffer.buffer, buffer.allocation);
    for (auto& buffer : morphWeightBuffers_)
        if (buffer.buffer) vmaDestroyBuffer(ctx_.allocator(), buffer.buffer, buffer.allocation);
    for (auto& buffer : clusterBuffers_)
        if (buffer.buffer) vmaDestroyBuffer(ctx_.allocator(), buffer.buffer, buffer.allocation);
    for (auto& buffer : clusterIndexBuffers_)
        if (buffer.buffer) vmaDestroyBuffer(ctx_.allocator(), buffer.buffer, buffer.allocation);
    vkDestroySampler(ctx_.device(), resolveSampler_, nullptr);
    vkDestroyImageView(ctx_.device(), defaultEnv_.cubeView, nullptr);
    vkDestroyImageView(ctx_.device(), defaultEnv_.lutView, nullptr);
    vmaDestroyImage(ctx_.allocator(), defaultEnv_.cube, defaultEnv_.cubeAllocation);
    vmaDestroyImage(ctx_.allocator(), defaultEnv_.lut, defaultEnv_.lutAllocation);
    vkDestroySampler(ctx_.device(), defaultEnv_.sampler, nullptr);
    vkDestroyImageView(ctx_.device(), probeArray_.arrayView, nullptr);
    vmaDestroyImage(ctx_.allocator(), probeArray_.image, probeArray_.allocation);
    vkDestroyPipeline(ctx_.device(), meshProbePipeline_, nullptr);
    vkDestroyPipeline(ctx_.device(), skyboxProbePipeline_, nullptr);
    vkDestroyPipeline(ctx_.device(), skyboxPipeline_, nullptr);
    vkDestroyPipeline(ctx_.device(), meshBlendPipeline_, nullptr);
    vkDestroyPipeline(ctx_.device(), meshPipeline_, nullptr);
    vkDestroyPipelineLayout(ctx_.device(), meshLayout_, nullptr);
    vkDestroyPipeline(ctx_.device(), tonemapPipeline_, nullptr);
    vkDestroyPipelineLayout(ctx_.device(), tonemapLayout_, nullptr);
    vkDestroyDescriptorPool(ctx_.device(), descriptorPool_, nullptr);
    vkDestroyDescriptorSetLayout(ctx_.device(), frameSetLayout_, nullptr);
}

void Renderer3D::createPipelines() {
    // ---- mesh pipeline
    VkShaderModule meshVert =
        createModule(ctx_.device(), meshVertBlob_.data, meshVertBlob_.words);
    VkShaderModule meshFrag =
        createModule(ctx_.device(), meshFragBlob_.data, meshFragBlob_.words);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = meshVert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = meshFrag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {4, 0, VK_FORMAT_R16G16B16A16_UINT, offsetof(Vertex, joints)},
        {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, weights)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 6;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    // Meshes are authored CCW-from-outside; through view + y-flipped
    // projection they land counter-clockwise in framebuffer space.
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = kSamples;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                            VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkFormat hdrFormat = kHdrFormat;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &hdrFormat;
    rendering.depthAttachmentFormat = kDepthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = meshLayout_;
    VK_CHECK(vkCreateGraphicsPipelines(ctx_.device(), ctx_.pipelineCache(), 1, &pipelineInfo, nullptr,
                                       &meshPipeline_));

    // Blend variant for AlphaMode::Blend: alpha blending, depth test but no
    // depth write (drawn back-to-front after the opaque pass).
    VkPipelineColorBlendAttachmentState blendOn = blendAttachment;
    blendOn.blendEnable = VK_TRUE;
    blendOn.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendOn.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendOn.colorBlendOp = VK_BLEND_OP_ADD;
    blendOn.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendOn.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendOn.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blendState = blend;
    blendState.pAttachments = &blendOn;
    VkPipelineDepthStencilStateCreateInfo blendDepth = depthStencil;
    blendDepth.depthWriteEnable = VK_FALSE;
    VkPipelineRasterizationStateCreateInfo blendRaster = raster;
    blendRaster.cullMode = VK_CULL_MODE_NONE; // see glass from both sides
    pipelineInfo.pColorBlendState = &blendState;
    pipelineInfo.pDepthStencilState = &blendDepth;
    pipelineInfo.pRasterizationState = &blendRaster;
    VK_CHECK(vkCreateGraphicsPipelines(ctx_.device(), ctx_.pipelineCache(), 1, &pipelineInfo,
                                       nullptr, &meshBlendPipeline_));
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pRasterizationState = &raster;

    // Probe-bake variant: single-sampled, CLOCKWISE front face. Probe faces
    // render with the point-shadow cube matrices (no y-flip), which mirrors
    // the framebuffer winding relative to the main pass.
    VkPipelineMultisampleStateCreateInfo probeSample{};
    probeSample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    probeSample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineRasterizationStateCreateInfo probeRaster = raster;
    probeRaster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    pipelineInfo.pMultisampleState = &probeSample;
    pipelineInfo.pRasterizationState = &probeRaster;
    VK_CHECK(vkCreateGraphicsPipelines(ctx_.device(), ctx_.pipelineCache(), 1, &pipelineInfo,
                                       nullptr, &meshProbePipeline_));
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pRasterizationState = &raster;

    vkDestroyShaderModule(ctx_.device(), meshVert, nullptr);
    vkDestroyShaderModule(ctx_.device(), meshFrag, nullptr);

    // ---- tonemap pipeline
    VkShaderModule tonemapVert =
        createModule(ctx_.device(), tonemapVertBlob_.data, tonemapVertBlob_.words);
    VkShaderModule tonemapFrag =
        createModule(ctx_.device(), tonemapFragBlob_.data, tonemapFragBlob_.words);
    stages[0].module = tonemapVert;
    stages[1].module = tonemapFrag;

    VkPipelineVertexInputStateCreateInfo emptyInput{};
    emptyInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineMultisampleStateCreateInfo singleSample{};
    singleSample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    singleSample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineRasterizationStateCreateInfo fullscreenRaster = raster;
    fullscreenRaster.cullMode = VK_CULL_MODE_NONE;
    VkPipelineRenderingCreateInfo tonemapRendering{};
    tonemapRendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    tonemapRendering.colorAttachmentCount = 1;
    tonemapRendering.pColorAttachmentFormats = &swapchainFormat_;

    pipelineInfo.pNext = &tonemapRendering;
    pipelineInfo.pVertexInputState = &emptyInput;
    pipelineInfo.pRasterizationState = &fullscreenRaster;
    pipelineInfo.pMultisampleState = &singleSample;
    pipelineInfo.pDepthStencilState = nullptr;
    pipelineInfo.layout = tonemapLayout_;
    VK_CHECK(vkCreateGraphicsPipelines(ctx_.device(), ctx_.pipelineCache(), 1, &pipelineInfo, nullptr,
                                       &tonemapPipeline_));
    vkDestroyShaderModule(ctx_.device(), tonemapVert, nullptr);
    vkDestroyShaderModule(ctx_.device(), tonemapFrag, nullptr);

    // ---- shadow pipeline (depth only, vertex shader only)
    VkShaderModule shadowVert =
        createModule(ctx_.device(), shadowVertBlob_.data, shadowVertBlob_.words);
    VkPipelineShaderStageCreateInfo shadowStage{};
    shadowStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shadowStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    shadowStage.module = shadowVert;
    shadowStage.pName = "main";

    const VkVertexInputAttributeDescription shadowAttributes[] = {
        attributes[0], attributes[4], attributes[5]}; // position + skinning
    VkPipelineVertexInputStateCreateInfo shadowInput{};
    shadowInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    shadowInput.vertexBindingDescriptionCount = 1;
    shadowInput.pVertexBindingDescriptions = &binding;
    shadowInput.vertexAttributeDescriptionCount = 3;
    shadowInput.pVertexAttributeDescriptions = shadowAttributes;

    VkPipelineRasterizationStateCreateInfo shadowRaster = raster;
    shadowRaster.cullMode = VK_CULL_MODE_NONE; // no peter-panning surprises
    shadowRaster.depthBiasEnable = VK_TRUE;
    shadowRaster.depthBiasConstantFactor = 1.25f;
    shadowRaster.depthBiasSlopeFactor = 1.75f;

    VkPipelineColorBlendStateCreateInfo noColor{};
    noColor.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

    VkPipelineRenderingCreateInfo shadowRendering{};
    shadowRendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    shadowRendering.depthAttachmentFormat = kDepthFormat;

    pipelineInfo.pNext = &shadowRendering;
    pipelineInfo.stageCount = 1;
    pipelineInfo.pStages = &shadowStage;
    pipelineInfo.pVertexInputState = &shadowInput;
    pipelineInfo.pRasterizationState = &shadowRaster;
    pipelineInfo.pMultisampleState = &singleSample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &noColor;
    pipelineInfo.layout = meshLayout_;
    VK_CHECK(vkCreateGraphicsPipelines(ctx_.device(), ctx_.pipelineCache(), 1, &pipelineInfo, nullptr,
                                       &shadowPipeline_));
    vkDestroyShaderModule(ctx_.device(), shadowVert, nullptr);

    // ---- skybox pipeline (fullscreen, drawn at far depth into the HDR pass)
    VkShaderModule skyboxVert =
        createModule(ctx_.device(), skyboxVertBlob_.data, skyboxVertBlob_.words);
    VkShaderModule skyboxFrag =
        createModule(ctx_.device(), skyboxFragBlob_.data, skyboxFragBlob_.words);
    VkPipelineShaderStageCreateInfo skyboxStages[2]{};
    skyboxStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    skyboxStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    skyboxStages[0].module = skyboxVert;
    skyboxStages[0].pName = "main";
    skyboxStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    skyboxStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    skyboxStages[1].module = skyboxFrag;
    skyboxStages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo skyboxInput{};
    skyboxInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineRasterizationStateCreateInfo skyboxRaster = raster;
    skyboxRaster.cullMode = VK_CULL_MODE_NONE;
    VkPipelineDepthStencilStateCreateInfo skyboxDepth = depthStencil;
    skyboxDepth.depthWriteEnable = VK_FALSE;
    skyboxDepth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL; // z == 1 passes clear
    VkPipelineRenderingCreateInfo skyboxRendering{};
    skyboxRendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    skyboxRendering.colorAttachmentCount = 1;
    VkFormat hdr = kHdrFormat;
    skyboxRendering.pColorAttachmentFormats = &hdr;
    skyboxRendering.depthAttachmentFormat = kDepthFormat;

    pipelineInfo.pNext = &skyboxRendering;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = skyboxStages;
    pipelineInfo.pVertexInputState = &skyboxInput;
    pipelineInfo.pRasterizationState = &skyboxRaster;
    pipelineInfo.pMultisampleState = &multisample; // MSAA like the scene pass
    pipelineInfo.pDepthStencilState = &skyboxDepth;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.layout = meshLayout_;
    VK_CHECK(vkCreateGraphicsPipelines(ctx_.device(), ctx_.pipelineCache(), 1, &pipelineInfo,
                                       nullptr, &skyboxPipeline_));

    // Single-sampled skybox for probe capture (CULL_NONE already; direction
    // comes from invViewProj so no winding concerns).
    pipelineInfo.pMultisampleState = &singleSample;
    VK_CHECK(vkCreateGraphicsPipelines(ctx_.device(), ctx_.pipelineCache(), 1, &pipelineInfo,
                                       nullptr, &skyboxProbePipeline_));

    vkDestroyShaderModule(ctx_.device(), skyboxVert, nullptr);
    vkDestroyShaderModule(ctx_.device(), skyboxFrag, nullptr);
}

void Renderer3D::destroyTargets() {
    for (Target* target : {&hdrMsaa_, &hdrResolve_, &depth_}) {
        if (target->view) vkDestroyImageView(ctx_.device(), target->view, nullptr);
        if (target->image) vmaDestroyImage(ctx_.allocator(), target->image, target->allocation);
        *target = {};
    }
    if (hdrBindlessIndex_ != 0) {
        bindless_.remove(hdrBindlessIndex_);
        hdrBindlessIndex_ = 0;
    }
}

void Renderer3D::recreateTargets(VkExtent2D extent, gpu::FrameRing& frames) {
    if (extent.width == targetExtent_.width && extent.height == targetExtent_.height) return;
    // Old targets may be in flight; a waitIdle on resize is acceptable.
    ctx_.waitIdle();
    destroyTargets();
    targetExtent_ = extent;

    auto createTarget = [&](Target* target, VkFormat format, VkSampleCountFlagBits samples,
                            VkImageUsageFlags usage, VkImageAspectFlags aspect) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {extent.width, extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = samples;
        imageInfo.usage = usage;
        VmaAllocationCreateInfo allocCreate{};
        allocCreate.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        allocCreate.priority = 1.0f;
        VK_CHECK(vmaCreateImage(ctx_.allocator(), &imageInfo, &allocCreate, &target->image,
                                &target->allocation, nullptr));
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = target->image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx_.device(), &viewInfo, nullptr, &target->view));
    };

    createTarget(&hdrMsaa_, kHdrFormat, kSamples,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT);
    createTarget(&hdrResolve_, kHdrFormat, VK_SAMPLE_COUNT_1_BIT,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT);
    createTarget(&depth_, kDepthFormat, kSamples, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT);

    hdrBindlessIndex_ = bindless_.add(hdrResolve_.view, resolveSampler_);
    (void)frames;
}

void Renderer3D::ensureCapacity(MappedBuffer& buffer, size_t bytes, uint32_t slot,
                                gpu::FrameRing& frames) {
    if (buffer.capacity >= bytes) return;
    size_t newCapacity = buffer.capacity == 0 ? 16 * 1024 : buffer.capacity;
    while (newCapacity < bytes) newCapacity *= 2;

    if (buffer.buffer != VK_NULL_HANDLE) {
        VkBuffer oldBuffer = buffer.buffer;
        VmaAllocation oldAllocation = buffer.allocation;
        VmaAllocator allocator = ctx_.allocator();
        frames.defer([allocator, oldBuffer, oldAllocation] {
            vmaDestroyBuffer(allocator, oldBuffer, oldAllocation);
        });
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = newCapacity;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
    allocCreate.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
    allocCreate.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VmaAllocationInfo allocInfo{};
    VK_CHECK(vmaCreateBuffer(ctx_.allocator(), &bufferInfo, &allocCreate, &buffer.buffer,
                             &buffer.allocation, &allocInfo));
    buffer.mapped = allocInfo.pMappedData;
    buffer.capacity = newCapacity;
    descriptorsDirty_[slot] = true;
}

void Renderer3D::updateDescriptors(uint32_t slot) {
    const EnvironmentData* env = boundEnvironment_;
    const VkSampler envSampler = env ? env->sampler : defaultEnv_.sampler;

    struct BufferBind {
        uint32_t binding;
        VkDescriptorType type;
        VkBuffer buffer;
        VkDeviceSize size;
    };
    const BufferBind bufferBinds[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uboBuffers_[slot].buffer, sizeof(FrameUbo)},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, transformBuffers_[slot].buffer, VK_WHOLE_SIZE},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, materialBuffers_[slot].buffer, VK_WHOLE_SIZE},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, lightBuffers_[slot].buffer, VK_WHOLE_SIZE},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, jointBuffers_[slot].buffer, VK_WHOLE_SIZE},
        {12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, boundMorphDeltaBuffer_, VK_WHOLE_SIZE},
        {13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, morphWeightBuffers_[slot].buffer,
         VK_WHOLE_SIZE},
        {14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, clusterBuffers_[slot].buffer, VK_WHOLE_SIZE},
        {15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, clusterIndexBuffers_[slot].buffer,
         VK_WHOLE_SIZE},
    };
    struct ImageBind {
        uint32_t binding;
        VkSampler sampler;
        VkImageView view;
        VkImageLayout layout;
    };
    const ImageBind imageBinds[] = {
        {4, shadowSampler_, cascadeShadows_.sampleView,
         VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
        {5, shadowSampler_, spotShadows_.sampleView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
        {6, pointShadowSampler_, pointShadows_.sampleView,
         VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
        {8, envSampler, env ? env->environment.view : defaultEnv_.cubeView,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {9, envSampler, env ? env->irradiance.view : defaultEnv_.cubeView,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {10, envSampler, env ? env->prefiltered.view : defaultEnv_.cubeView,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {11, envSampler, env ? env->brdfLutView : defaultEnv_.lutView,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {16, defaultEnv_.sampler, probeArray_.arrayView,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };

    VkDescriptorBufferInfo bufferInfos[std::size(bufferBinds)];
    VkDescriptorImageInfo imageInfos[std::size(imageBinds)];
    VkWriteDescriptorSet writes[std::size(bufferBinds) + std::size(imageBinds)]{};
    uint32_t writeCount = 0;
    for (size_t i = 0; i < std::size(bufferBinds); ++i) {
        bufferInfos[i] = {bufferBinds[i].buffer, 0, bufferBinds[i].size};
        VkWriteDescriptorSet& write = writes[writeCount++];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = frameSets_[slot];
        write.dstBinding = bufferBinds[i].binding;
        write.descriptorCount = 1;
        write.descriptorType = bufferBinds[i].type;
        write.pBufferInfo = &bufferInfos[i];
    }
    for (size_t i = 0; i < std::size(imageBinds); ++i) {
        imageInfos[i] = {imageBinds[i].sampler, imageBinds[i].view, imageBinds[i].layout};
        VkWriteDescriptorSet& write = writes[writeCount++];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = frameSets_[slot];
        write.dstBinding = imageBinds[i].binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfos[i];
    }
    vkUpdateDescriptorSets(ctx_.device(), writeCount, writes, 0, nullptr);
    descriptorsDirty_[slot] = false;
}

void Renderer3D::renderShadowPass(VkCommandBuffer cmd, const ShadowArray& array, uint32_t layer,
                                  const Mat4& lightViewProj, SceneImpl& scene,
                                  const std::vector<ShadowGroup>& groups) {
    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = array.layerViews[layer];
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea = {{0, 0}, {array.size, array.size}};
    renderingInfo.layerCount = 1;
    renderingInfo.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(cmd, &renderingInfo);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(array.size),
                              static_cast<float>(array.size), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, {array.size, array.size}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    for (const ShadowGroup& group : groups) {
        const MeshRange& range = scene.meshes->range(group.mesh);
        ShadowPush push{};
        push.lightViewProj = lightViewProj;
        push.transformIndex = group.baseTransform;
        push.jointBase = group.jointBase;
        push.morphWeightBase = group.morphWeightBase;
        push.morphDeltaBase = range.morphDeltaBase;
        push.morphTargetCount = group.morphTargetCount;
        push.meshVertexBase = static_cast<uint32_t>(range.vertexOffset);
        push.meshVertexCount = range.vertexCount;
        vkCmdPushConstants(cmd, meshLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDrawIndexed(cmd, range.indexCount, group.instances, range.firstIndex,
                         range.vertexOffset, 0);
    }
    vkCmdEndRendering(cmd);
}

void Renderer3D::render(VkCommandBuffer cmd, uint32_t slot, SceneImpl& scene,
                        const Camera& camera, VkExtent2D extent, VkImageView swapchainView,
                        gpu::FrameRing& frames) {
    recreateTargets(extent, frames);
    scene.updateWorldTransforms();

    const float aspect =
        static_cast<float>(extent.width) / static_cast<float>(std::max(extent.height, 1u));

    // ---- gather visible mesh nodes
    FrameUbo ubo{};
    ubo.view = camera.view();
    ubo.proj = camera.proj(aspect);
    ubo.viewProj = ubo.proj * ubo.view;
    ubo.invViewProj = glm::inverse(ubo.viewProj);
    ubo.viewPos = Vec4{camera.position, 1.0f};
    ubo.ambient =
        Vec4{scene.ambient.r, scene.ambient.g, scene.ambient.b, scene.environmentIntensity};

    // Rebind descriptors when the environment changes.
    if (scene.environment.get() != boundEnvironment_) {
        boundEnvironment_ = scene.environment.get();
        for (bool& dirty : descriptorsDirty_) dirty = true;
    }
    ubo.counts.z = boundEnvironment_ != nullptr ? 1u : 0u;
    ubo.pointShadowParams.y = static_cast<float>(kPrefilterMips - 1);

    // Reflection probes: slot i in the UBO owns cube array layers i*6..i*6+5,
    // so slots keep their index for the probe's lifetime (freed slots are
    // reused by addReflectionProbe). The cube array holds ONE scene's bake —
    // a scene that isn't the current owner gets no probes (its stale `baked`
    // flags would otherwise sample another scene's capture).
    const uint32_t probeCount =
        scene.sceneId == probeOwnerScene_
            ? std::min(static_cast<uint32_t>(scene.probes.size()), kMaxProbes)
            : 0u;
    ubo.pointShadowParams.z = static_cast<float>(probeCount);
    ubo.pointShadowParams.w = static_cast<float>(kProbeMips - 1);
    for (uint32_t p = 0; p < probeCount; ++p) {
        const ReflectionProbeData& probe = scene.probes[p];
        ubo.probePositions[p] =
            Vec4{probe.position, probe.alive && probe.baked ? 1.0f : 0.0f};
        ubo.probeBoxMins[p] = Vec4{probe.boxMin, probe.fade};
        ubo.probeBoxMaxs[p] = Vec4{probe.boxMax, 0.0f};
    }

    const FrustumPlanes frustum(ubo.viewProj);

    // Transforms for every alive mesh node (shadow passes see off-screen
    // casters); the main pass draws the frustum-culled subset.
    struct DrawItem {
        uint32_t transformIndex;
        uint32_t materialIndex;
        MeshHandle mesh;
        float viewDepth;     // blend sorting
        uint32_t instances;  // opaque groups
        uint32_t jointBase = kNoJoints;
        uint32_t morphWeightBase = 0;
        uint32_t morphTargetCount = 0;
    };
    std::vector<Mat4> transforms;
    std::vector<DrawItem> blendDraws;
    // Everything is grouped for instancing: shadow groups hold ALL alive
    // opaque casters (mesh-keyed), camera groups hold the frustum-culled
    // visible set (mesh+material-keyed); blended nodes draw individually.
    std::map<uint32_t, std::vector<Mat4>> shadowGroupBuild;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<Mat4>> opaqueGroups;
    std::vector<DrawItem> skinnedDraws;
    std::vector<ShadowGroup> skinnedShadowDraws;
    std::vector<Mat4> jointMatrices;
    std::vector<float> frameMorphWeights;
    // Caching per skinIndex is correct even when several mesh nodes share a
    // skin: per the glTF spec the skinned mesh node's own transform MUST be
    // ignored, so every user of a skin gets the identical world-space pose
    // (jointWorld * inverseBind). Separate loadGltf calls create separate
    // skins, so no false sharing across model instances either.
    std::map<int32_t, uint32_t> jointBaseOfSkin; // built once per skin per frame
    auto jointBaseFor = [&](int32_t skinIndex) -> uint32_t {
        if (auto it = jointBaseOfSkin.find(skinIndex); it != jointBaseOfSkin.end())
            return it->second;
        const Skin& skin = scene.skins[static_cast<size_t>(skinIndex)];
        const auto base = static_cast<uint32_t>(jointMatrices.size());
        for (size_t j = 0; j < skin.jointNodes.size(); ++j) {
            const uint32_t jointNode = skin.jointNodes[j];
            const Mat4 world =
                jointNode < scene.nodes.size() ? scene.nodes[jointNode].world : Mat4{1.0f};
            jointMatrices.push_back(world * skin.inverseBind[j]);
        }
        jointBaseOfSkin.emplace(skinIndex, base);
        return base;
    };

    for (uint32_t i = 0; i < scene.nodes.size(); ++i) {
        const SceneNode& node = scene.nodes[i];
        if (!node.alive || !node.mesh.valid() || !scene.meshes->valid(node.mesh)) continue;

        const bool blends = node.material.id < scene.materialAlphaModes.size() &&
                            scene.materialAlphaModes[node.material.id] == AlphaMode::Blend;

        const MeshRange& nodeRange = scene.meshes->range(node.mesh);
        const bool skinned = node.skinIndex >= 0 &&
                             static_cast<size_t>(node.skinIndex) < scene.skins.size();
        const bool morphed = nodeRange.morphTargetCount > 0;
        if (skinned || morphed) {
            // Skinned/morphed: pose comes from joints/weights — no
            // instancing, and skinned meshes skip frustum culling (their
            // animated bounds leave the bind pose).
            const uint32_t jointBase = skinned ? jointBaseFor(node.skinIndex) : kNoJoints;
            uint32_t morphWeightBase = 0;
            if (morphed) {
                morphWeightBase = static_cast<uint32_t>(frameMorphWeights.size());
                for (uint32_t t = 0; t < nodeRange.morphTargetCount; ++t)
                    frameMorphWeights.push_back(
                        t < node.morphWeights.size() ? node.morphWeights[t] : 0.0f);
            }
            DrawItem item{0,         node.material.id, node.mesh,
                          0.0f,      1,                jointBase,
                          morphWeightBase, nodeRange.morphTargetCount};
            if (!skinned) {
                // Morph-only nodes still follow their node transform.
                item.transformIndex = static_cast<uint32_t>(transforms.size());
                transforms.push_back(node.world);
            }
            if (blends) {
                const Vec3 worldCenter = Vec3(node.world[3]);
                item.viewDepth = -(ubo.view * Vec4{worldCenter, 1.0f}).z;
                blendDraws.push_back(item);
            } else {
                skinnedDraws.push_back(item);
                skinnedShadowDraws.push_back({item.transformIndex, 1, node.mesh, jointBase,
                                              morphWeightBase, nodeRange.morphTargetCount});
            }
            continue;
        }

        if (!blends) shadowGroupBuild[node.mesh.id].push_back(node.world);

        const MeshRange& range = scene.meshes->range(node.mesh);
        const Vec3 worldCenter = Vec3(node.world * Vec4{range.boundsCenter, 1.0f});
        const Vec3 scale{glm::length(Vec3(node.world[0])), glm::length(Vec3(node.world[1])),
                         glm::length(Vec3(node.world[2]))};
        const float worldRadius =
            range.boundsRadius * std::max(scale.x, std::max(scale.y, scale.z));
        if (!frustum.visible(worldCenter, worldRadius)) continue;

        if (blends) {
            const float viewDepth = -(ubo.view * Vec4{worldCenter, 1.0f}).z;
            blendDraws.push_back({static_cast<uint32_t>(transforms.size()), node.material.id,
                                  node.mesh, viewDepth, 1});
            transforms.push_back(node.world);
        } else {
            opaqueGroups[{node.mesh.id, node.material.id}].push_back(node.world);
        }
    }
    std::vector<ShadowGroup> shadowGroups;
    shadowGroups.reserve(shadowGroupBuild.size());
    for (auto& [meshId, matrices] : shadowGroupBuild) {
        shadowGroups.push_back({static_cast<uint32_t>(transforms.size()),
                                static_cast<uint32_t>(matrices.size()), MeshHandle{meshId}});
        transforms.insert(transforms.end(), matrices.begin(), matrices.end());
    }
    shadowGroups.insert(shadowGroups.end(), skinnedShadowDraws.begin(),
                        skinnedShadowDraws.end());
    std::vector<DrawItem> draws;
    draws.reserve(opaqueGroups.size());
    for (auto& [key, matrices] : opaqueGroups) {
        draws.push_back({static_cast<uint32_t>(transforms.size()), key.second,
                         MeshHandle{key.first}, 0.0f,
                         static_cast<uint32_t>(matrices.size())});
        transforms.insert(transforms.end(), matrices.begin(), matrices.end());
    }
    // Transparent surfaces draw farthest-first over the opaque result.
    std::sort(blendDraws.begin(), blendDraws.end(),
              [](const DrawItem& a, const DrawItem& b) { return a.viewDepth > b.viewDepth; });

    // ---- lights + shadow assignment
    struct SpotShadowJob {
        uint32_t layer;
        Mat4 matrix;
    };
    struct PointShadowJob {
        uint32_t cubeIndex;
        Vec3 position;
        float range;
    };
    std::vector<GpuLight> directionalLights;
    std::vector<GpuLight> localLights; // point/spot: clustered
    std::vector<SpotShadowJob> spotJobs;
    std::vector<PointShadowJob> pointJobs;
    Vec3 sunDirection{0.0f, -1.0f, 0.0f};
    bool haveSun = false;
    uint32_t spotShadowCount = 0;
    uint32_t pointShadowCount = 0;
    localLights.reserve(scene.lights.size());
    for (size_t i = 0; i < scene.lights.size(); ++i) {
        const Light& light = scene.lights[i];
        const SceneNode& node = scene.nodes[scene.lightNodes[i]];
        if (!node.alive) continue;
        GpuLight gpuLight{};
        const Vec3 worldPos = Vec3(node.world[3]) + light.position;
        const Vec3 worldDir = glm::normalize(Mat3(node.world) * light.direction);
        if (light.type == Light::Type::Directional)
            gpuLight.positionType = Vec4{worldDir, 0.0f};
        else
            gpuLight.positionType =
                Vec4{worldPos, light.type == Light::Type::Point ? 1.0f : 2.0f};
        gpuLight.colorIntensity =
            Vec4{light.color.r, light.color.g, light.color.b, light.intensity};
        gpuLight.directionRange = Vec4{worldDir, light.range};

        float shadowIndex = -1.0f;
        if (light.castsShadows) {
            switch (light.type) {
            case Light::Type::Directional:
                if (!haveSun) { // one shadowed sun in v1
                    haveSun = true;
                    sunDirection = worldDir;
                    shadowIndex = 0.0f;
                }
                break;
            case Light::Type::Spot:
                if (spotShadowCount < kMaxSpotShadows) {
                    const float range = light.range > 0.0f ? light.range : 50.0f;
                    const Mat4 proj = glm::perspective(2.0f * light.outerCone, 1.0f, 0.1f, range);
                    const Vec3 up = std::abs(worldDir.y) > 0.99f ? Vec3{1.0f, 0.0f, 0.0f}
                                                                 : Vec3{0.0f, 1.0f, 0.0f};
                    const Mat4 matrix =
                        proj * glm::lookAt(worldPos, worldPos + worldDir, up);
                    ubo.spotMatrices[spotShadowCount] = matrix;
                    spotJobs.push_back({spotShadowCount, matrix});
                    shadowIndex = static_cast<float>(spotShadowCount++);
                }
                break;
            case Light::Type::Point:
                if (pointShadowCount < kMaxPointShadows) {
                    pointJobs.push_back({pointShadowCount, worldPos,
                                         light.range > 0.0f ? light.range : 50.0f});
                    shadowIndex = static_cast<float>(pointShadowCount++);
                }
                break;
            }
        }
        gpuLight.cone =
            Vec4{std::cos(light.innerCone), std::cos(light.outerCone), shadowIndex, 0.0f};
        if (light.type == Light::Type::Directional)
            directionalLights.push_back(gpuLight);
        else
            localLights.push_back(gpuLight);
    }
    // Directionals first (always shaded), then the clustered local lights.
    std::vector<GpuLight> gpuLights = std::move(directionalLights);
    const auto directionalCount = static_cast<uint32_t>(gpuLights.size());
    gpuLights.insert(gpuLights.end(), localLights.begin(), localLights.end());
    ubo.counts.x = static_cast<uint32_t>(gpuLights.size());
    ubo.counts.w = directionalCount;
    ubo.pointShadowParams.x = kPointShadowNear;

    // ---- forward+ cluster assignment (CPU: lights are few, clusters cheap)
    const float clusterNear = std::max(camera.nearPlane, 0.01f);
    const float clusterFar = std::max(camera.farPlane, clusterNear + 1.0f);
    const float logRatio = std::log2(clusterFar / clusterNear);
    const float sliceScale = static_cast<float>(kClusterZ) / logRatio;
    const float sliceBias =
        static_cast<float>(kClusterZ) * std::log2(clusterNear) / logRatio;
    ubo.clusterParams = {static_cast<float>(extent.width) / kClusterX,
                         static_cast<float>(extent.height) / kClusterY, sliceScale, sliceBias};

    clusterScratch_.resize(kClusterCount);
    for (auto& list : clusterScratch_) list.clear();
    const auto sliceOf = [&](float depth) {
        return static_cast<int>(
            std::floor(std::log2(std::max(depth, clusterNear)) * sliceScale - sliceBias));
    };
    for (uint32_t lightIndex = directionalCount; lightIndex < gpuLights.size(); ++lightIndex) {
        const GpuLight& light = gpuLights[lightIndex];
        const Vec3 viewPos = Vec3(ubo.view * Vec4{Vec3(light.positionType), 1.0f});
        const float radius =
            light.directionRange.w > 0.0f ? light.directionRange.w : clusterFar;
        const float depth = -viewPos.z;
        if (depth + radius < clusterNear || depth - radius > clusterFar) continue;

        const int z0 = std::clamp(sliceOf(depth - radius), 0, static_cast<int>(kClusterZ) - 1);
        const int z1 = std::clamp(sliceOf(depth + radius), 0, static_cast<int>(kClusterZ) - 1);

        // Conservative x/y tile range from the projected view-space AABB.
        int x0 = static_cast<int>(kClusterX) - 1, x1 = 0;
        int y0 = static_cast<int>(kClusterY) - 1, y1 = 0;
        bool fullExtent = false;
        for (int corner = 0; corner < 8 && !fullExtent; ++corner) {
            const Vec3 offset{(corner & 1) ? radius : -radius,
                              (corner & 2) ? radius : -radius,
                              (corner & 4) ? radius : -radius};
            const Vec4 clip = ubo.proj * Vec4{viewPos + offset, 1.0f};
            if (clip.w <= 0.001f) {
                fullExtent = true; // crosses the near plane: cover everything
                break;
            }
            const Vec2 ndc = Vec2(clip) / clip.w;
            const int tx = static_cast<int>((ndc.x * 0.5f + 0.5f) * kClusterX);
            const int ty = static_cast<int>((ndc.y * 0.5f + 0.5f) * kClusterY);
            x0 = std::min(x0, tx);
            x1 = std::max(x1, tx);
            y0 = std::min(y0, ty);
            y1 = std::max(y1, ty);
        }
        if (fullExtent) {
            x0 = y0 = 0;
            x1 = static_cast<int>(kClusterX) - 1;
            y1 = static_cast<int>(kClusterY) - 1;
        } else {
            x0 = std::clamp(x0, 0, static_cast<int>(kClusterX) - 1);
            x1 = std::clamp(x1, 0, static_cast<int>(kClusterX) - 1);
            y0 = std::clamp(y0, 0, static_cast<int>(kClusterY) - 1);
            y1 = std::clamp(y1, 0, static_cast<int>(kClusterY) - 1);
            if (x1 < x0 || y1 < y0) continue; // fully off-screen
        }
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    clusterScratch_[static_cast<size_t>(x) +
                                    kClusterX * (static_cast<size_t>(y) +
                                                 kClusterY * static_cast<size_t>(z))]
                        .push_back(lightIndex);
    }

    // Flatten to (offset, count) + index list. No hard cap, but huge lists
    // (many unbounded-range lights) make fragments expensive — warn once.
    std::vector<glm::uvec2> clusterRanges(kClusterCount);
    std::vector<uint32_t> clusterIndices;
    for (uint32_t c = 0; c < kClusterCount; ++c) {
        clusterRanges[c] = {static_cast<uint32_t>(clusterIndices.size()),
                            static_cast<uint32_t>(clusterScratch_[c].size())};
        clusterIndices.insert(clusterIndices.end(), clusterScratch_[c].begin(),
                              clusterScratch_[c].end());
    }
    if (clusterIndices.size() > 500'000) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            log::warn("forward+: {} cluster light entries — give large point/spot lights a "
                      "finite .range to keep shading cheap",
                      clusterIndices.size());
        }
    }

    // ---- cascaded shadow matrices for the sun
    if (haveSun) {
        ubo.counts.y = kMaxCascades;
        const float nearPlane = camera.nearPlane;
        const float farPlane = std::min(camera.farPlane, shadowDistance);
        const float tanHalfFov = std::tan(camera.fovY * 0.5f);
        const Mat4 invView = glm::inverse(ubo.view);

        float splitNear = nearPlane;
        for (uint32_t cascade = 0; cascade < kMaxCascades; ++cascade) {
            // Practical split scheme (λ = 0.75).
            const float p = static_cast<float>(cascade + 1) / kMaxCascades;
            const float logSplit = nearPlane * std::pow(farPlane / nearPlane, p);
            const float uniformSplit = nearPlane + (farPlane - nearPlane) * p;
            const float splitFar = glm::mix(uniformSplit, logSplit, 0.75f);

            // Sphere-fit the sub-frustum (stable under rotation).
            Vec3 corners[8];
            int cornerIndex = 0;
            for (float depth : {splitNear, splitFar}) {
                const float y = depth * tanHalfFov;
                const float x = y * aspect;
                for (const Vec2 sign : {Vec2{-1, -1}, Vec2{1, -1}, Vec2{-1, 1}, Vec2{1, 1}})
                    corners[cornerIndex++] =
                        Vec3(invView * Vec4{sign.x * x, sign.y * y, -depth, 1.0f});
            }
            Vec3 center{0.0f};
            for (const Vec3& corner : corners) center += corner;
            center /= 8.0f;
            float radius = 0.0f;
            for (const Vec3& corner : corners)
                radius = std::max(radius, glm::length(corner - center));
            radius = std::ceil(radius * 16.0f) / 16.0f;

            const Vec3 up = std::abs(sunDirection.y) > 0.99f ? Vec3{1.0f, 0.0f, 0.0f}
                                                             : Vec3{0.0f, 1.0f, 0.0f};
            const float zPadding = 40.0f; // room for casters behind the frustum
            const Mat4 lightView =
                glm::lookAt(center - sunDirection * (radius + zPadding), center, up);
            const Mat4 lightProj =
                glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + zPadding);
            Mat4 lightViewProj = lightProj * lightView;

            // Texel snap: keep the origin on the shadow texel grid.
            const float mapSize = static_cast<float>(cascadeShadows_.size);
            Vec4 origin = lightViewProj * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
            origin *= mapSize / 2.0f;
            const Vec4 rounded = glm::round(origin);
            Vec4 offset = (rounded - origin) * 2.0f / mapSize;
            lightViewProj[3].x += offset.x;
            lightViewProj[3].y += offset.y;

            ubo.cascadeMatrices[cascade] = lightViewProj;
            ubo.cascadeSplits[static_cast<int>(cascade)] = splitFar;
            splitNear = splitFar;
        }
    }

    // ---- upload per-frame data
    ensureCapacity(uboBuffers_[slot], sizeof(FrameUbo), slot, frames);
    std::memcpy(uboBuffers_[slot].mapped, &ubo, sizeof(FrameUbo));
    if (!transforms.empty()) {
        ensureCapacity(transformBuffers_[slot], transforms.size() * sizeof(Mat4), slot, frames);
        std::memcpy(transformBuffers_[slot].mapped, transforms.data(),
                    transforms.size() * sizeof(Mat4));
    } else {
        ensureCapacity(transformBuffers_[slot], sizeof(Mat4), slot, frames);
    }
    ensureCapacity(materialBuffers_[slot],
                   std::max<size_t>(scene.materials.size(), 1) * sizeof(GpuMaterial), slot,
                   frames);
    std::memcpy(materialBuffers_[slot].mapped, scene.materials.data(),
                scene.materials.size() * sizeof(GpuMaterial));
    ensureCapacity(lightBuffers_[slot], std::max<size_t>(gpuLights.size(), 1) * sizeof(GpuLight),
                   slot, frames);
    if (!gpuLights.empty())
        std::memcpy(lightBuffers_[slot].mapped, gpuLights.data(),
                    gpuLights.size() * sizeof(GpuLight));
    ensureCapacity(clusterBuffers_[slot], kClusterCount * sizeof(glm::uvec2), slot, frames);
    std::memcpy(clusterBuffers_[slot].mapped, clusterRanges.data(),
                kClusterCount * sizeof(glm::uvec2));
    ensureCapacity(clusterIndexBuffers_[slot],
                   std::max<size_t>(clusterIndices.size(), 1) * sizeof(uint32_t), slot,
                   frames);
    if (!clusterIndices.empty())
        std::memcpy(clusterIndexBuffers_[slot].mapped, clusterIndices.data(),
                    clusterIndices.size() * sizeof(uint32_t));
    ensureCapacity(jointBuffers_[slot],
                   std::max<size_t>(jointMatrices.size(), 1) * sizeof(Mat4), slot, frames);
    if (!jointMatrices.empty())
        std::memcpy(jointBuffers_[slot].mapped, jointMatrices.data(),
                    jointMatrices.size() * sizeof(Mat4));
    ensureCapacity(morphWeightBuffers_[slot],
                   std::max<size_t>(frameMorphWeights.size(), 1) * sizeof(float), slot, frames);
    if (!frameMorphWeights.empty())
        std::memcpy(morphWeightBuffers_[slot].mapped, frameMorphWeights.data(),
                    frameMorphWeights.size() * sizeof(float));
    if (scene.meshes->morphDeltaBuffer() != boundMorphDeltaBuffer_) {
        boundMorphDeltaBuffer_ = scene.meshes->morphDeltaBuffer();
        for (bool& dirty : descriptorsDirty_) dirty = true;
    }
    if (descriptorsDirty_[slot]) updateDescriptors(slot);

    // ---- shadow passes
    const bool anyShadows = haveSun || !spotJobs.empty() || !pointJobs.empty();
    for (ShadowArray* array : {&cascadeShadows_, &spotShadows_, &pointShadows_}) {
        gpu::imageBarrier(cmd, array->image,
                          shadowsInSampleLayout_ ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    if (anyShadows) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
        const VkDescriptorSet shadowSets[] = {bindless_.set(), frameSets_[slot]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout_, 0, 2,
                                shadowSets, 0, nullptr);
        const VkDeviceSize zeroOffset = 0;
        VkBuffer vertexBuffer = scene.meshes->vertexBuffer();
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &zeroOffset);
        vkCmdBindIndexBuffer(cmd, scene.meshes->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);

        if (haveSun)
            for (uint32_t cascade = 0; cascade < kMaxCascades; ++cascade)
                renderShadowPass(cmd, cascadeShadows_, cascade, ubo.cascadeMatrices[cascade],
                                 scene, shadowGroups);
        for (const SpotShadowJob& job : spotJobs)
            renderShadowPass(cmd, spotShadows_, job.layer, job.matrix, scene, shadowGroups);
        for (const PointShadowJob& job : pointJobs) {
            const Mat4 proj =
                glm::perspective(glm::radians(90.0f), 1.0f, kPointShadowNear, job.range);
            const struct {
                Vec3 dir;
                Vec3 up;
            } faces[6] = {
                {{1, 0, 0}, {0, -1, 0}},  {{-1, 0, 0}, {0, -1, 0}}, {{0, 1, 0}, {0, 0, 1}},
                {{0, -1, 0}, {0, 0, -1}}, {{0, 0, 1}, {0, -1, 0}},  {{0, 0, -1}, {0, -1, 0}},
            };
            for (uint32_t face = 0; face < 6; ++face) {
                const Mat4 view =
                    glm::lookAt(job.position, job.position + faces[face].dir, faces[face].up);
                renderShadowPass(cmd, pointShadows_, job.cubeIndex * 6 + face, proj * view,
                                 scene, shadowGroups);
            }
        }
    }

    for (ShadowArray* array : {&cascadeShadows_, &spotShadows_, &pointShadows_}) {
        gpu::imageBarrier(cmd, array->image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    shadowsInSampleLayout_ = true;

    // ---- scene pass (HDR MSAA + depth, resolve at end)
    gpu::imageBarrier(cmd, hdrMsaa_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    gpu::imageBarrier(cmd, hdrResolve_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    gpu::imageBarrier(cmd, depth_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = hdrMsaa_.view;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    colorAttachment.resolveImageView = hdrResolve_.view;
    colorAttachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = depth_.view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea = {{0, 0}, extent};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(cmd, &renderingInfo);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                              static_cast<float>(extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (!draws.empty() || !skinnedDraws.empty() || !blendDraws.empty() ||
        boundEnvironment_ != nullptr) {
        const VkDescriptorSet sets[] = {bindless_.set(), frameSets_[slot]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout_, 0, 2, sets, 0,
                                nullptr);
        const VkDeviceSize zeroOffset = 0;
        VkBuffer vertexBuffer = scene.meshes->vertexBuffer();
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &zeroOffset);
        vkCmdBindIndexBuffer(cmd, scene.meshes->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);

        auto recordDraws = [&](const std::vector<DrawItem>& items) {
            for (const DrawItem& draw : items) {
                const MeshRange& range = scene.meshes->range(draw.mesh);
                const MeshPush push{draw.transformIndex,
                                    draw.materialIndex,
                                    draw.jointBase,
                                    draw.morphWeightBase,
                                    range.morphDeltaBase,
                                    draw.morphTargetCount,
                                    static_cast<uint32_t>(range.vertexOffset),
                                    range.vertexCount};
                vkCmdPushConstants(cmd, meshLayout_,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(push), &push);
                vkCmdDrawIndexed(cmd, range.indexCount, draw.instances, range.firstIndex,
                                 range.vertexOffset, 0);
            }
        };
        if (!draws.empty() || !skinnedDraws.empty()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline_);
            recordDraws(draws);
            recordDraws(skinnedDraws);
        }
        if (boundEnvironment_ != nullptr) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipeline_);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        if (!blendDraws.empty()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshBlendPipeline_);
            recordDraws(blendDraws);
        }
    }
    vkCmdEndRendering(cmd);

    // ---- tonemap: resolve texture → swapchain
    gpu::imageBarrier(cmd, hdrResolve_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    VkRenderingAttachmentInfo swapchainAttachment{};
    swapchainAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    swapchainAttachment.imageView = swapchainView;
    swapchainAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapchainAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    swapchainAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo tonemapInfo{};
    tonemapInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    tonemapInfo.renderArea = {{0, 0}, extent};
    tonemapInfo.layerCount = 1;
    tonemapInfo.colorAttachmentCount = 1;
    tonemapInfo.pColorAttachments = &swapchainAttachment;
    vkCmdBeginRendering(cmd, &tonemapInfo);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipeline_);
    VkDescriptorSet bindlessSet = bindless_.set();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapLayout_, 0, 1,
                            &bindlessSet, 0, nullptr);
    const TonemapPush tonemapPush{hdrBindlessIndex_, static_cast<uint32_t>(tonemapper),
                                  exposure};
    vkCmdPushConstants(cmd, tonemapLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(tonemapPush), &tonemapPush);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void Renderer3D::bakeProbes(SceneImpl& scene) {
    std::vector<uint32_t> slots;
    for (uint32_t i = 0; i < scene.probes.size() && i < kMaxProbes; ++i)
        if (scene.probes[i].alive) slots.push_back(i);
    if (slots.empty()) return;

    if (probeOwnerScene_ != 0 && probeOwnerScene_ != scene.sceneId)
        log::debug("bakeReflectionProbes: probe array taken over by another scene — "
                   "the previous scene's probes deactivate until it re-bakes");
    probeOwnerScene_ = scene.sceneId;

    // Blocking one-shot work: nothing may be in flight while we reuse the
    // shadow arrays and rewrite the probe array.
    ctx_.waitIdle();
    scene.updateWorldTransforms();

    // ---- gather draws: every alive non-blend mesh node, individual draws
    struct BakeDraw {
        uint32_t transformIndex;
        uint32_t materialIndex;
        MeshHandle mesh;
        uint32_t jointBase;
        uint32_t morphWeightBase;
        uint32_t morphTargetCount;
    };
    std::vector<Mat4> transforms;
    std::vector<Mat4> jointMatrices;
    std::vector<float> morphWeights;
    std::vector<BakeDraw> draws;
    std::map<int32_t, uint32_t> jointBaseOfSkin;
    for (uint32_t i = 0; i < scene.nodes.size(); ++i) {
        const SceneNode& node = scene.nodes[i];
        if (!node.alive || !node.mesh.valid() || !scene.meshes->valid(node.mesh)) continue;
        if (node.material.id < scene.materialAlphaModes.size() &&
            scene.materialAlphaModes[node.material.id] == AlphaMode::Blend)
            continue; // transparent surfaces are skipped in probe captures
        const MeshRange& range = scene.meshes->range(node.mesh);
        const bool skinned = node.skinIndex >= 0 &&
                             static_cast<size_t>(node.skinIndex) < scene.skins.size();
        uint32_t jointBase = kNoJoints;
        if (skinned) {
            if (auto it = jointBaseOfSkin.find(node.skinIndex); it != jointBaseOfSkin.end()) {
                jointBase = it->second;
            } else {
                const Skin& skin = scene.skins[static_cast<size_t>(node.skinIndex)];
                jointBase = static_cast<uint32_t>(jointMatrices.size());
                for (size_t j = 0; j < skin.jointNodes.size(); ++j) {
                    const uint32_t jointNode = skin.jointNodes[j];
                    const Mat4 world = jointNode < scene.nodes.size()
                                           ? scene.nodes[jointNode].world
                                           : Mat4{1.0f};
                    jointMatrices.push_back(world * skin.inverseBind[j]);
                }
                jointBaseOfSkin.emplace(node.skinIndex, jointBase);
            }
        }
        uint32_t morphWeightBase = 0;
        if (range.morphTargetCount > 0) {
            morphWeightBase = static_cast<uint32_t>(morphWeights.size());
            for (uint32_t t = 0; t < range.morphTargetCount; ++t)
                morphWeights.push_back(t < node.morphWeights.size() ? node.morphWeights[t]
                                                                   : 0.0f);
        }
        draws.push_back({static_cast<uint32_t>(transforms.size()), node.material.id, node.mesh,
                         jointBase, morphWeightBase, range.morphTargetCount});
        transforms.push_back(node.world);
    }

    // ---- lights: everything shades, nothing samples shadow maps
    std::vector<GpuLight> directionalLights;
    std::vector<GpuLight> localLights;
    for (size_t i = 0; i < scene.lights.size(); ++i) {
        const Light& light = scene.lights[i];
        const SceneNode& node = scene.nodes[scene.lightNodes[i]];
        if (!node.alive) continue;
        GpuLight gpuLight{};
        const Vec3 worldPos = Vec3(node.world[3]) + light.position;
        const Vec3 worldDir = glm::normalize(Mat3(node.world) * light.direction);
        if (light.type == Light::Type::Directional)
            gpuLight.positionType = Vec4{worldDir, 0.0f};
        else
            gpuLight.positionType =
                Vec4{worldPos, light.type == Light::Type::Point ? 1.0f : 2.0f};
        gpuLight.colorIntensity =
            Vec4{light.color.r, light.color.g, light.color.b, light.intensity};
        gpuLight.directionRange = Vec4{worldDir, light.range};
        gpuLight.cone =
            Vec4{std::cos(light.innerCone), std::cos(light.outerCone), -1.0f, 0.0f};
        (light.type == Light::Type::Directional ? directionalLights : localLights)
            .push_back(gpuLight);
    }
    std::vector<GpuLight> gpuLights = std::move(directionalLights);
    const auto directionalCount = static_cast<uint32_t>(gpuLights.size());
    gpuLights.insert(gpuLights.end(), localLights.begin(), localLights.end());

    // Degenerate cluster grid: every fragment lands in cluster 0, which
    // holds every local light (probe faces are 128² — brute force is fine).
    std::vector<glm::uvec2> clusterRanges(kClusterCount, glm::uvec2{0, 0});
    std::vector<uint32_t> clusterIndices;
    for (uint32_t l = directionalCount; l < gpuLights.size(); ++l) clusterIndices.push_back(l);
    clusterRanges[0] = {0, static_cast<uint32_t>(clusterIndices.size())};

    // ---- host-visible bake buffers
    struct BakeBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };
    std::vector<BakeBuffer> bakeBuffers;
    bakeBuffers.reserve(8); // the returned references must stay valid
    auto createMapped = [&](const void* data, size_t bytes) -> BakeBuffer& {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = std::max<size_t>(bytes, 16);
        bufferInfo.usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo allocCreate{};
        allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreate.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT;
        allocCreate.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VmaAllocationInfo allocInfo{};
        BakeBuffer buffer;
        VK_CHECK(vmaCreateBuffer(ctx_.allocator(), &bufferInfo, &allocCreate, &buffer.buffer,
                                 &buffer.allocation, &allocInfo));
        buffer.mapped = allocInfo.pMappedData;
        if (data != nullptr && bytes > 0) std::memcpy(buffer.mapped, data, bytes);
        bakeBuffers.push_back(buffer);
        return bakeBuffers.back();
    };

    BakeBuffer& uboBuffer = createMapped(nullptr, sizeof(FrameUbo));
    BakeBuffer& transformBuffer =
        createMapped(transforms.data(), transforms.size() * sizeof(Mat4));
    BakeBuffer& materialBuffer =
        createMapped(scene.materials.data(), scene.materials.size() * sizeof(GpuMaterial));
    BakeBuffer& lightBuffer = createMapped(gpuLights.data(), gpuLights.size() * sizeof(GpuLight));
    BakeBuffer& jointBuffer =
        createMapped(jointMatrices.data(), jointMatrices.size() * sizeof(Mat4));
    BakeBuffer& morphWeightBuffer =
        createMapped(morphWeights.data(), morphWeights.size() * sizeof(float));
    BakeBuffer& clusterBuffer =
        createMapped(clusterRanges.data(), clusterRanges.size() * sizeof(glm::uvec2));
    BakeBuffer& clusterIndexBuffer =
        createMapped(clusterIndices.data(), clusterIndices.size() * sizeof(uint32_t));

    // ---- bake descriptor set (mirrors updateDescriptors, bake-local buffers)
    const EnvironmentData* env = scene.environment.get();
    const VkSampler envSampler = env ? env->sampler : defaultEnv_.sampler;
    {
        const VkDescriptorBufferInfo bufferInfos[] = {
            {uboBuffer.buffer, 0, sizeof(FrameUbo)},
            {transformBuffer.buffer, 0, VK_WHOLE_SIZE},
            {materialBuffer.buffer, 0, VK_WHOLE_SIZE},
            {lightBuffer.buffer, 0, VK_WHOLE_SIZE},
            {jointBuffer.buffer, 0, VK_WHOLE_SIZE},
            {scene.meshes->morphDeltaBuffer(), 0, VK_WHOLE_SIZE},
            {morphWeightBuffer.buffer, 0, VK_WHOLE_SIZE},
            {clusterBuffer.buffer, 0, VK_WHOLE_SIZE},
            {clusterIndexBuffer.buffer, 0, VK_WHOLE_SIZE},
        };
        const uint32_t bufferBindings[] = {0, 1, 2, 3, 7, 12, 13, 14, 15};
        const VkDescriptorImageInfo imageInfos[] = {
            {shadowSampler_, cascadeShadows_.sampleView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
            {shadowSampler_, spotShadows_.sampleView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
            {pointShadowSampler_, pointShadows_.sampleView,
             VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
            {envSampler, env ? env->environment.view : defaultEnv_.cubeView,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {envSampler, env ? env->irradiance.view : defaultEnv_.cubeView,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {envSampler, env ? env->prefiltered.view : defaultEnv_.cubeView,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {envSampler, env ? env->brdfLutView : defaultEnv_.lutView,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {defaultEnv_.sampler, probeArray_.arrayView,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        const uint32_t imageBindings[] = {4, 5, 6, 8, 9, 10, 11, 16};
        VkWriteDescriptorSet writes[std::size(bufferInfos) + std::size(imageInfos)]{};
        uint32_t writeCount = 0;
        for (size_t i = 0; i < std::size(bufferInfos); ++i) {
            VkWriteDescriptorSet& write = writes[writeCount++];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = bakeSet_;
            write.dstBinding = bufferBindings[i];
            write.descriptorCount = 1;
            write.descriptorType = bufferBindings[i] == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                          : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &bufferInfos[i];
        }
        for (size_t i = 0; i < std::size(imageInfos); ++i) {
            VkWriteDescriptorSet& write = writes[writeCount++];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = bakeSet_;
            write.dstBinding = imageBindings[i];
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfos[i];
        }
        vkUpdateDescriptorSets(ctx_.device(), writeCount, writes, 0, nullptr);
    }

    // ---- scratch capture cube (mips for the prefilter's blurred taps) + depth
    EnvironmentData::CubeImage capture;
    {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = kHdrFormat;
        info.extent = {kProbeSize, kProbeSize, 1};
        info.mipLevels = kProbeMips;
        info.arrayLayers = 6;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocCreate{};
        allocCreate.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VK_CHECK(vmaCreateImage(ctx_.allocator(), &info, &allocCreate, &capture.image,
                                &capture.allocation, nullptr));
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = capture.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        viewInfo.format = kHdrFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kProbeMips, 0, 6};
        VK_CHECK(vkCreateImageView(ctx_.device(), &viewInfo, nullptr, &capture.view));
    }
    Target scratchDepth;
    {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = kDepthFormat;
        info.extent = {kProbeSize, kProbeSize, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        VmaAllocationCreateInfo allocCreate{};
        allocCreate.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VK_CHECK(vmaCreateImage(ctx_.allocator(), &info, &allocCreate, &scratchDepth.image,
                                &scratchDepth.allocation, nullptr));
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = scratchDepth.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kDepthFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx_.device(), &viewInfo, nullptr, &scratchDepth.view));
    }

    EnvBaker baker(ctx_, env_bake_vert_spv, env_bake_vert_spv_words);
    VkPipeline prefilterPipeline = baker.makePipeline(
        env_prefilter_frag_spv, env_prefilter_frag_spv_words, kHdrFormat);
    VkDescriptorSet captureSet = baker.makeInputSet(capture.view, defaultEnv_.sampler);

    // Cube face capture matrices — same convention as the point shadow cubes
    // (glm::perspective without the main pass's y-flip; the probe pipelines
    // compensate the mirrored winding with a CLOCKWISE front face).
    const struct {
        Vec3 dir;
        Vec3 up;
    } faces[6] = {
        {{1, 0, 0}, {0, -1, 0}},  {{-1, 0, 0}, {0, -1, 0}}, {{0, 1, 0}, {0, 0, 1}},
        {{0, -1, 0}, {0, 0, -1}}, {{0, 0, 1}, {0, -1, 0}},  {{0, 0, -1}, {0, -1, 0}},
    };
    const Mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, 1000.0f);

    bool firstSubmit = true;
    for (uint32_t slot : slots) {
        ReflectionProbeData& probe = scene.probes[slot];

        // ---- capture: one blocking submit per face (the UBO is reused)
        for (uint32_t face = 0; face < 6; ++face) {
            FrameUbo ubo{};
            ubo.view = glm::lookAt(probe.position, probe.position + faces[face].dir,
                                   faces[face].up);
            ubo.proj = proj;
            ubo.viewProj = ubo.proj * ubo.view;
            ubo.invViewProj = glm::inverse(ubo.viewProj);
            ubo.viewPos = Vec4{probe.position, 1.0f};
            ubo.ambient = Vec4{scene.ambient.r, scene.ambient.g, scene.ambient.b,
                               scene.environmentIntensity};
            ubo.counts = {static_cast<uint32_t>(gpuLights.size()), 0u,
                          env != nullptr ? 1u : 0u, directionalCount};
            ubo.pointShadowParams = {kPointShadowNear, static_cast<float>(kPrefilterMips - 1),
                                     0.0f, static_cast<float>(kProbeMips - 1)};
            // Degenerate grid: every fragment maps to cluster 0.
            ubo.clusterParams = {1e9f, 1e9f, 0.0f, 0.0f};
            std::memcpy(uboBuffer.mapped, &ubo, sizeof(FrameUbo));

            baker.begin();
            if (firstSubmit) {
                firstSubmit = false;
                if (!shadowsInSampleLayout_) {
                    // Never rendered: move the (garbage, never sampled)
                    // shadow arrays to the layout the descriptors declare.
                    for (ShadowArray* array : {&cascadeShadows_, &spotShadows_, &pointShadows_})
                        gpu::imageBarrier(baker.cmd, array->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                                          VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                          VK_IMAGE_ASPECT_DEPTH_BIT);
                    shadowsInSampleLayout_ = true;
                }
            }
            if (face == 0)
                envWholeImageBarrier(baker.cmd, capture.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            gpu::imageBarrier(baker.cmd, scratchDepth.image, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                              VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView =
                baker.faceView(capture.image, face, 0, kHdrFormat);
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            VkRenderingAttachmentInfo depthAttachment{};
            depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAttachment.imageView = scratchDepth.view;
            depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAttachment.clearValue.depthStencil = {1.0f, 0};
            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = {{0, 0}, {kProbeSize, kProbeSize}};
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;
            renderingInfo.pDepthAttachment = &depthAttachment;
            vkCmdBeginRendering(baker.cmd, &renderingInfo);
            const VkViewport viewport{0.0f, 0.0f, static_cast<float>(kProbeSize),
                                      static_cast<float>(kProbeSize), 0.0f, 1.0f};
            vkCmdSetViewport(baker.cmd, 0, 1, &viewport);
            const VkRect2D scissor{{0, 0}, {kProbeSize, kProbeSize}};
            vkCmdSetScissor(baker.cmd, 0, 1, &scissor);

            const VkDescriptorSet sets[] = {bindless_.set(), bakeSet_};
            vkCmdBindDescriptorSets(baker.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout_, 0,
                                    2, sets, 0, nullptr);
            const VkDeviceSize zeroOffset = 0;
            VkBuffer vertexBuffer = scene.meshes->vertexBuffer();
            vkCmdBindVertexBuffers(baker.cmd, 0, 1, &vertexBuffer, &zeroOffset);
            vkCmdBindIndexBuffer(baker.cmd, scene.meshes->indexBuffer(), 0,
                                 VK_INDEX_TYPE_UINT32);
            vkCmdBindPipeline(baker.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshProbePipeline_);
            for (const BakeDraw& draw : draws) {
                const MeshRange& range = scene.meshes->range(draw.mesh);
                const MeshPush push{draw.transformIndex,
                                    draw.materialIndex,
                                    draw.jointBase,
                                    draw.morphWeightBase,
                                    range.morphDeltaBase,
                                    draw.morphTargetCount,
                                    static_cast<uint32_t>(range.vertexOffset),
                                    range.vertexCount};
                vkCmdPushConstants(baker.cmd, meshLayout_,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(push), &push);
                vkCmdDrawIndexed(baker.cmd, range.indexCount, 1, range.firstIndex,
                                 range.vertexOffset, 0);
            }
            if (env != nullptr) {
                vkCmdBindPipeline(baker.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  skyboxProbePipeline_);
                vkCmdDraw(baker.cmd, 3, 1, 0, 0);
            }
            vkCmdEndRendering(baker.cmd);
            baker.submitAndWait();
        }

        // ---- mip chain + GGX prefilter into the probe's array layers
        baker.begin();
        envWholeImageBarrier(baker.cmd, capture.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        int32_t mipSize = static_cast<int32_t>(kProbeSize);
        for (uint32_t mip = 1; mip < kProbeMips; ++mip) {
            VkImageMemoryBarrier2 toSrc{};
            toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toSrc.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            toSrc.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            toSrc.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            toSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toSrc.image = capture.image;
            toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1, 0, 6};
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &toSrc;
            vkCmdPipelineBarrier2(baker.cmd, &dep);

            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 6};
            blit.srcOffsets[1] = {mipSize, mipSize, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 6};
            blit.dstOffsets[1] = {std::max(mipSize / 2, 1), std::max(mipSize / 2, 1), 1};
            vkCmdBlitImage(baker.cmd, capture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           capture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_LINEAR);
            mipSize = std::max(mipSize / 2, 1);
        }
        {
            // Mips [0, N-1) are TRANSFER_SRC, the last is TRANSFER_DST.
            VkImageMemoryBarrier2 barriers[2]{};
            for (auto& barrier : barriers) {
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.srcAccessMask =
                    VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.image = capture.image;
            }
            barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kProbeMips - 1, 0, 6};
            barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, kProbeMips - 1, 1, 0, 6};
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 2;
            dep.pImageMemoryBarriers = barriers;
            vkCmdPipelineBarrier2(baker.cmd, &dep);
        }

        // This probe's 6 layers: SHADER_READ → COLOR_ATTACHMENT → prefilter
        // → SHADER_READ.
        auto probeLayersBarrier = [&](VkImageLayout oldLayout, VkImageLayout newLayout) {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.image = probeArray_.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kProbeMips, slot * 6, 6};
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(baker.cmd, &dep);
        };
        probeLayersBarrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        for (uint32_t mip = 0; mip < kProbeMips; ++mip) {
            const uint32_t size = kProbeSize >> mip;
            const float roughness = static_cast<float>(mip) / (kProbeMips - 1);
            for (uint32_t face = 0; face < 6; ++face)
                baker.renderFace(prefilterPipeline, captureSet,
                                 baker.faceView(probeArray_.image, slot * 6 + face, mip,
                                                kHdrFormat),
                                 size, face, roughness);
        }
        probeLayersBarrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        baker.submitAndWait();

        probe.baked = true;
    }

    // All submits are fenced — safe to destroy the scratch resources now.
    vkDestroyImageView(ctx_.device(), capture.view, nullptr);
    vmaDestroyImage(ctx_.allocator(), capture.image, capture.allocation);
    vkDestroyImageView(ctx_.device(), scratchDepth.view, nullptr);
    vmaDestroyImage(ctx_.allocator(), scratchDepth.image, scratchDepth.allocation);
    for (BakeBuffer& buffer : bakeBuffers)
        vmaDestroyBuffer(ctx_.allocator(), buffer.buffer, buffer.allocation);

    log::info("probes: baked {} reflection probe(s) ({} draws each, {}² × {} mips)",
              slots.size(), draws.size(), kProbeSize, kProbeMips);
}

} // namespace rendy::detail
