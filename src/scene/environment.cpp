#include "scene/environment.hpp"

#include "rendy/core/log.hpp"

#include "shaders/env_bake_vert_spv.h"
#include "shaders/env_brdf_lut_frag_spv.h"
#include "shaders/env_equirect_frag_spv.h"
#include "shaders/env_irradiance_frag_spv.h"
#include "shaders/env_prefilter_frag_spv.h"

#include <stb_image.h>

#include <cstring>
#include <vector>

namespace rendy::detail {
namespace {

constexpr VkFormat kEnvFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kLutFormat = VK_FORMAT_R16G16_SFLOAT;

struct BakePush {
    uint32_t face;
    float roughness;
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

// Small helper owning everything the bake needs and cleaning it up.
struct Baker {
    gpu::Context& ctx;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule vertModule = VK_NULL_HANDLE;
    std::vector<VkPipeline> pipelines;
    std::vector<VkImageView> scratchViews;
    std::vector<VkSampler> scratchSamplers;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    explicit Baker(gpu::Context& context) : ctx(context) {
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &layoutInfo, nullptr, &setLayout));

        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 8;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        VK_CHECK(vkCreateDescriptorPool(ctx.device(), &poolInfo, nullptr, &pool));

        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BakePush)};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &push;
        VK_CHECK(vkCreatePipelineLayout(ctx.device(), &pipelineLayoutInfo, nullptr,
                                        &pipelineLayout));

        vertModule = createModule(ctx.device(), env_bake_vert_spv, env_bake_vert_spv_words);

        VkCommandPoolCreateInfo cmdPoolInfo{};
        cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        cmdPoolInfo.queueFamilyIndex = ctx.graphicsFamily();
        VK_CHECK(vkCreateCommandPool(ctx.device(), &cmdPoolInfo, nullptr, &commandPool));
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &allocInfo, &cmd));
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK(vkCreateFence(ctx.device(), &fenceInfo, nullptr, &fence));
    }

    ~Baker() {
        for (VkPipeline pipeline : pipelines)
            vkDestroyPipeline(ctx.device(), pipeline, nullptr);
        for (VkImageView view : scratchViews)
            vkDestroyImageView(ctx.device(), view, nullptr);
        for (VkSampler sampler : scratchSamplers)
            vkDestroySampler(ctx.device(), sampler, nullptr);
        vkDestroyShaderModule(ctx.device(), vertModule, nullptr);
        vkDestroyPipelineLayout(ctx.device(), pipelineLayout, nullptr);
        vkDestroyDescriptorPool(ctx.device(), pool, nullptr);
        vkDestroyDescriptorSetLayout(ctx.device(), setLayout, nullptr);
        vkDestroyFence(ctx.device(), fence, nullptr);
        vkDestroyCommandPool(ctx.device(), commandPool, nullptr);
    }

    VkPipeline makePipeline(const uint32_t* fragSpv, size_t fragWords, VkFormat colorFormat) {
        VkShaderModule frag = createModule(ctx.device(), fragSpv, fragWords);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
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
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = pipelineLayout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), ctx.pipelineCache(), 1, &info, nullptr,
                                           &pipeline));
        vkDestroyShaderModule(ctx.device(), frag, nullptr);
        pipelines.push_back(pipeline);
        return pipeline;
    }

    VkDescriptorSet makeInputSet(VkImageView view, VkSampler sampler) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &allocInfo, &set));
        VkDescriptorImageInfo imageInfo{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
        return set;
    }

    // Face view for rendering into (layer, mip) of a cube image.
    VkImageView faceView(VkImage image, uint32_t face, uint32_t mip, VkFormat format) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format;
        info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, face, 1};
        VkImageView view = VK_NULL_HANDLE;
        VK_CHECK(vkCreateImageView(ctx.device(), &info, nullptr, &view));
        scratchViews.push_back(view);
        return view;
    }

    void renderFace(VkPipeline pipeline, VkDescriptorSet input, VkImageView target,
                    uint32_t size, uint32_t face, float roughness) {
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = target;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, {size, size}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &rendering);
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(size),
                                  static_cast<float>(size), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        const VkRect2D scissor{{0, 0}, {size, size}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        if (input != VK_NULL_HANDLE)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
                                    &input, 0, nullptr);
        const BakePush push{face, roughness};
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                           &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }

    void submitAndWait() {
        VK_CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, fence));
        VK_CHECK(vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX));
    }
};

void createCube(gpu::Context& ctx, EnvironmentData::CubeImage* cube, uint32_t size,
                uint32_t mips, VkImageUsageFlags extraUsage = 0) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = kEnvFormat;
    info.extent = {size, size, 1};
    info.mipLevels = mips;
    info.arrayLayers = 6;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extraUsage;
    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &info, &allocCreate, &cube->image,
                            &cube->allocation, nullptr));
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = cube->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = kEnvFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 6};
    VK_CHECK(vkCreateImageView(ctx.device(), &viewInfo, nullptr, &cube->view));
}

void wholeImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                       VkImageLayout newLayout) {
    gpu::imageBarrier(cmd, image, oldLayout, newLayout, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT);
}

} // namespace

EnvironmentData::~EnvironmentData() {
    for (CubeImage* cube : {&environment, &irradiance, &prefiltered}) {
        if (cube->view) vkDestroyImageView(ctx_->device(), cube->view, nullptr);
        if (cube->image) vmaDestroyImage(ctx_->allocator(), cube->image, cube->allocation);
    }
    if (brdfLutView) vkDestroyImageView(ctx_->device(), brdfLutView, nullptr);
    if (brdfLut) vmaDestroyImage(ctx_->allocator(), brdfLut, brdfLutAllocation);
    if (sampler) vkDestroySampler(ctx_->device(), sampler, nullptr);
}

Result<std::shared_ptr<EnvironmentData>> bakeEnvironment(gpu::Context& ctx,
                                                         const std::string& hdrPath) {
    // ---- load the equirect HDR
    int width = 0;
    int height = 0;
    int channels = 0;
    float* pixels = stbi_loadf(hdrPath.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr)
        return err("environment: failed to load '{}': {}", hdrPath, stbi_failure_reason());

    auto env = std::make_shared<EnvironmentData>(ctx);
    Baker baker(ctx);

    // Equirect source texture (RGBA32F).
    VkImage equirectImage = VK_NULL_HANDLE;
    VmaAllocation equirectAlloc = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocCreate{};
        allocCreate.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &info, &allocCreate, &equirectImage,
                                &equirectAlloc, nullptr));

        const size_t bytes = static_cast<size_t>(width) * height * 4 * sizeof(float);
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo stagingCreate{};
        stagingCreate.usage = VMA_MEMORY_USAGE_AUTO;
        stagingCreate.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                              VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo stagingInfo{};
        VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bufferInfo, &stagingCreate, &staging,
                                 &stagingAlloc, &stagingInfo));
        std::memcpy(stagingInfo.pMappedData, pixels, bytes);
    }
    stbi_image_free(pixels);

    createCube(ctx, &env->environment, kEnvCubeSize, kEnvCubeMips,
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    createCube(ctx, &env->irradiance, kIrradianceSize, 1);
    createCube(ctx, &env->prefiltered, kPrefilterSize, kPrefilterMips);
    {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = kLutFormat;
        info.extent = {kBrdfLutSize, kBrdfLutSize, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo allocCreate{};
        allocCreate.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &info, &allocCreate, &env->brdfLut,
                                &env->brdfLutAllocation, nullptr));
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = env->brdfLut;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kLutFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx.device(), &viewInfo, nullptr, &env->brdfLutView));
    }
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        VK_CHECK(vkCreateSampler(ctx.device(), &samplerInfo, nullptr, &env->sampler));
    }

    // ---- pipelines
    VkPipeline equirectPipeline = baker.makePipeline(
        env_equirect_frag_spv, env_equirect_frag_spv_words, kEnvFormat);
    VkPipeline irradiancePipeline = baker.makePipeline(
        env_irradiance_frag_spv, env_irradiance_frag_spv_words, kEnvFormat);
    VkPipeline prefilterPipeline = baker.makePipeline(
        env_prefilter_frag_spv, env_prefilter_frag_spv_words, kEnvFormat);
    VkPipeline lutPipeline =
        baker.makePipeline(env_brdf_lut_frag_spv, env_brdf_lut_frag_spv_words, kLutFormat);

    VkImageView equirectView = baker.faceView(equirectImage, 0, 0,
                                              VK_FORMAT_R32G32B32A32_SFLOAT);
    VkDescriptorSet equirectSet = baker.makeInputSet(equirectView, env->sampler);
    VkDescriptorSet envSet = baker.makeInputSet(env->environment.view, env->sampler);

    // ---- record
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(baker.cmd, &beginInfo));

    // Upload equirect.
    wholeImageBarrier(baker.cmd, equirectImage, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    vkCmdCopyBufferToImage(baker.cmd, staging, equirectImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    wholeImageBarrier(baker.cmd, equirectImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Equirect → env cube mip 0.
    wholeImageBarrier(baker.cmd, env->environment.image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    for (uint32_t face = 0; face < 6; ++face)
        baker.renderFace(equirectPipeline, equirectSet,
                         baker.faceView(env->environment.image, face, 0, kEnvFormat),
                         kEnvCubeSize, face, 0.0f);

    // Mip chain for the env cube (blit).
    wholeImageBarrier(baker.cmd, env->environment.image,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    int32_t mipSize = static_cast<int32_t>(kEnvCubeSize);
    for (uint32_t mip = 1; mip < kEnvCubeMips; ++mip) {
        VkImageMemoryBarrier2 toSrc{};
        toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toSrc.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toSrc.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        toSrc.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.image = env->environment.image;
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
        vkCmdBlitImage(baker.cmd, env->environment.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       env->environment.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
        mipSize = std::max(mipSize / 2, 1);
    }
    // After the blit chain, mips [0, N-1) are TRANSFER_SRC and the last is
    // TRANSFER_DST — transition each range without discarding contents.
    {
        VkImageMemoryBarrier2 barriers[2]{};
        for (auto& barrier : barriers) {
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask =
                VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.image = env->environment.image;
        }
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kEnvCubeMips - 1, 0, 6};
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, kEnvCubeMips - 1, 1, 0, 6};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 2;
        dep.pImageMemoryBarriers = barriers;
        vkCmdPipelineBarrier2(baker.cmd, &dep);
    }

    // Irradiance.
    wholeImageBarrier(baker.cmd, env->irradiance.image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    for (uint32_t face = 0; face < 6; ++face)
        baker.renderFace(irradiancePipeline, envSet,
                         baker.faceView(env->irradiance.image, face, 0, kEnvFormat),
                         kIrradianceSize, face, 0.0f);
    wholeImageBarrier(baker.cmd, env->irradiance.image,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Prefiltered chain.
    wholeImageBarrier(baker.cmd, env->prefiltered.image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    for (uint32_t mip = 0; mip < kPrefilterMips; ++mip) {
        const uint32_t size = kPrefilterSize >> mip;
        const float roughness = static_cast<float>(mip) / (kPrefilterMips - 1);
        for (uint32_t face = 0; face < 6; ++face)
            baker.renderFace(prefilterPipeline, envSet,
                             baker.faceView(env->prefiltered.image, face, mip, kEnvFormat),
                             size, face, roughness);
    }
    wholeImageBarrier(baker.cmd, env->prefiltered.image,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // BRDF LUT.
    wholeImageBarrier(baker.cmd, env->brdfLut, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    baker.renderFace(lutPipeline, VK_NULL_HANDLE, env->brdfLutView, kBrdfLutSize, 0, 0.0f);
    wholeImageBarrier(baker.cmd, env->brdfLut, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    baker.submitAndWait();

    vmaDestroyBuffer(ctx.allocator(), staging, stagingAlloc);
    vmaDestroyImage(ctx.allocator(), equirectImage, equirectAlloc);

    log::info("environment: baked '{}' ({}x{} HDR → {}³ cube, {} prefilter mips)", hdrPath,
              width, height, kEnvCubeSize, kPrefilterMips);
    return env;
}

} // namespace rendy::detail
