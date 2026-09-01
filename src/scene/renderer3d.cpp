#include "scene/renderer3d.hpp"

#include "shaders/mesh_frag_spv.h"
#include "shaders/mesh_vert_spv.h"
#include "shaders/shadow_vert_spv.h"
#include "shaders/tonemap_frag_spv.h"
#include "shaders/tonemap_vert_spv.h"

#include <cstring>

namespace rendy::detail {
namespace {

// Matches shaders/scene_common.glsl FrameData.
struct FrameUbo {
    Mat4 view;
    Mat4 proj;
    Mat4 viewProj;
    Mat4 cascadeMatrices[Renderer3D::kMaxCascades];
    Mat4 spotMatrices[Renderer3D::kMaxSpotShadows];
    Vec4 cascadeSplits;
    Vec4 viewPos;
    Vec4 ambient;
    glm::uvec4 counts;
    Vec4 pointShadowParams;
};

struct MeshPush {
    uint32_t transformIndex;
    uint32_t materialIndex;
};

struct ShadowPush {
    Mat4 lightViewProj;
    uint32_t transformIndex;
    uint32_t pad[3];
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
        planes[4] = m[3] + m[2]; // near (0..1 depth: w + z? see below)
        planes[5] = m[3] - m[2]; // far
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
                       VkFormat swapchainFormat)
    : ctx_(ctx), bindless_(bindless) {
    // Set 1: UBO + transforms + materials + lights + 3 shadow map arrays.
    VkDescriptorSetLayoutBinding bindings[7]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (uint32_t i = 1; i < 4; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (uint32_t i = 4; i < 7; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 7;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx_.device(), &layoutInfo, nullptr, &frameSetLayout_));

    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, gpu::kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * gpu::kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * gpu::kFramesInFlight},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = gpu::kFramesInFlight;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx_.device(), &poolInfo, nullptr, &descriptorPool_));

    VkDescriptorSetLayout layouts[gpu::kFramesInFlight];
    for (auto& layout : layouts) layout = frameSetLayout_;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = gpu::kFramesInFlight;
    allocInfo.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(ctx_.device(), &allocInfo, frameSets_));

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

    createPipelines(swapchainFormat);
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
    vkDestroySampler(ctx_.device(), resolveSampler_, nullptr);
    vkDestroyPipeline(ctx_.device(), meshPipeline_, nullptr);
    vkDestroyPipelineLayout(ctx_.device(), meshLayout_, nullptr);
    vkDestroyPipeline(ctx_.device(), tonemapPipeline_, nullptr);
    vkDestroyPipelineLayout(ctx_.device(), tonemapLayout_, nullptr);
    vkDestroyDescriptorPool(ctx_.device(), descriptorPool_, nullptr);
    vkDestroyDescriptorSetLayout(ctx_.device(), frameSetLayout_, nullptr);
}

void Renderer3D::createPipelines(VkFormat swapchainFormat) {
    // ---- mesh pipeline layout
    // Sized for the largest user (ShadowPush); the mesh pass uses 8 bytes.
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

    // ---- mesh pipeline
    VkShaderModule meshVert = createModule(ctx_.device(), mesh_vert_spv, mesh_vert_spv_words);
    VkShaderModule meshFrag = createModule(ctx_.device(), mesh_frag_spv, mesh_frag_spv_words);

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
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 4;
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
    vkDestroyShaderModule(ctx_.device(), meshVert, nullptr);
    vkDestroyShaderModule(ctx_.device(), meshFrag, nullptr);

    // ---- tonemap pipeline
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

    VkShaderModule tonemapVert =
        createModule(ctx_.device(), tonemap_vert_spv, tonemap_vert_spv_words);
    VkShaderModule tonemapFrag =
        createModule(ctx_.device(), tonemap_frag_spv, tonemap_frag_spv_words);
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
    tonemapRendering.pColorAttachmentFormats = &swapchainFormat;

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
        createModule(ctx_.device(), shadow_vert_spv, shadow_vert_spv_words);
    VkPipelineShaderStageCreateInfo shadowStage{};
    shadowStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shadowStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    shadowStage.module = shadowVert;
    shadowStage.pName = "main";

    VkPipelineVertexInputStateCreateInfo shadowInput{};
    shadowInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    shadowInput.vertexBindingDescriptionCount = 1;
    shadowInput.pVertexBindingDescriptions = &binding;
    shadowInput.vertexAttributeDescriptionCount = 1; // position only
    shadowInput.pVertexAttributeDescriptions = attributes;

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
    VkDescriptorBufferInfo bufferInfos[4] = {
        {uboBuffers_[slot].buffer, 0, sizeof(FrameUbo)},
        {transformBuffers_[slot].buffer, 0, VK_WHOLE_SIZE},
        {materialBuffers_[slot].buffer, 0, VK_WHOLE_SIZE},
        {lightBuffers_[slot].buffer, 0, VK_WHOLE_SIZE},
    };
    VkDescriptorImageInfo imageInfos[3] = {
        {shadowSampler_, cascadeShadows_.sampleView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
        {shadowSampler_, spotShadows_.sampleView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
        {pointShadowSampler_, pointShadows_.sampleView,
         VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet writes[7]{};
    for (uint32_t i = 0; i < 7; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = frameSets_[slot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        if (i < 4) {
            writes[i].descriptorType =
                i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &bufferInfos[i];
        } else {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &imageInfos[i - 4];
        }
    }
    vkUpdateDescriptorSets(ctx_.device(), 7, writes, 0, nullptr);
    descriptorsDirty_[slot] = false;
}

void Renderer3D::renderShadowPass(VkCommandBuffer cmd, const ShadowArray& array, uint32_t layer,
                                  const Mat4& lightViewProj, SceneImpl& scene,
                                  const std::vector<uint32_t>& transformIndexOfNode) {
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

    for (uint32_t i = 0; i < scene.nodes.size(); ++i) {
        const SceneNode& node = scene.nodes[i];
        if (!node.alive || !node.mesh.valid()) continue;
        ShadowPush push{};
        push.lightViewProj = lightViewProj;
        push.transformIndex = transformIndexOfNode[i];
        vkCmdPushConstants(cmd, meshLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        const MeshRange& range = scene.meshes->range(node.mesh);
        vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, range.vertexOffset, 0);
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
    ubo.viewPos = Vec4{camera.position, 1.0f};
    ubo.ambient = Vec4{scene.ambient.r, scene.ambient.g, scene.ambient.b, 1.0f};

    const FrustumPlanes frustum(ubo.viewProj);

    // Transforms for every alive mesh node (shadow passes see off-screen
    // casters); the main pass draws the frustum-culled subset.
    struct DrawItem {
        uint32_t transformIndex;
        uint32_t materialIndex;
        MeshHandle mesh;
    };
    std::vector<Mat4> transforms;
    std::vector<DrawItem> draws;
    std::vector<uint32_t> transformIndexOfNode(scene.nodes.size(), 0);
    transforms.reserve(scene.nodes.size());
    for (uint32_t i = 0; i < scene.nodes.size(); ++i) {
        const SceneNode& node = scene.nodes[i];
        if (!node.alive || !node.mesh.valid()) continue;
        transformIndexOfNode[i] = static_cast<uint32_t>(transforms.size());
        transforms.push_back(node.world);

        const MeshRange& range = scene.meshes->range(node.mesh);
        const Vec3 worldCenter = Vec3(node.world * Vec4{range.boundsCenter, 1.0f});
        const Vec3 scale{glm::length(Vec3(node.world[0])), glm::length(Vec3(node.world[1])),
                         glm::length(Vec3(node.world[2]))};
        const float worldRadius =
            range.boundsRadius * std::max(scale.x, std::max(scale.y, scale.z));
        if (!frustum.visible(worldCenter, worldRadius)) continue;
        draws.push_back({transformIndexOfNode[i], node.material.id, node.mesh});
    }

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
    std::vector<GpuLight> gpuLights;
    std::vector<SpotShadowJob> spotJobs;
    std::vector<PointShadowJob> pointJobs;
    Vec3 sunDirection{0.0f, -1.0f, 0.0f};
    bool haveSun = false;
    uint32_t spotShadowCount = 0;
    uint32_t pointShadowCount = 0;
    gpuLights.reserve(scene.lights.size());
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
        gpuLights.push_back(gpuLight);
    }
    ubo.counts.x = static_cast<uint32_t>(gpuLights.size());
    ubo.pointShadowParams.x = kPointShadowNear;

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
                                 scene, transformIndexOfNode);
        for (const SpotShadowJob& job : spotJobs)
            renderShadowPass(cmd, spotShadows_, job.layer, job.matrix, scene,
                             transformIndexOfNode);
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
                                 scene, transformIndexOfNode);
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

    if (!draws.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline_);
        const VkDescriptorSet sets[] = {bindless_.set(), frameSets_[slot]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout_, 0, 2, sets, 0,
                                nullptr);
        const VkDeviceSize zeroOffset = 0;
        VkBuffer vertexBuffer = scene.meshes->vertexBuffer();
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &zeroOffset);
        vkCmdBindIndexBuffer(cmd, scene.meshes->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);

        for (const DrawItem& draw : draws) {
            const MeshPush push{draw.transformIndex, draw.materialIndex};
            vkCmdPushConstants(cmd, meshLayout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            const MeshRange& range = scene.meshes->range(draw.mesh);
            vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, range.vertexOffset, 0);
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

} // namespace rendy::detail
