#include "canvas/renderer2d.hpp"

#include "shaders/quad2d_frag_spv.h"
#include "shaders/quad2d_vert_spv.h"

#include <cstring>

namespace rendy::detail {
namespace {

VkShaderModule createModule(VkDevice device, const uint32_t* code, size_t words) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = words * sizeof(uint32_t);
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

} // namespace

Renderer2D::Renderer2D(gpu::Context& ctx, gpu::BindlessTable& bindless, VkFormat colorFormat)
    : ctx_(ctx), bindless_(bindless) {
    // Set 1: quad SSBO + clip SSBO, one set per frame in flight.
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx_.device(), &layoutInfo, nullptr, &frameSetLayout_));

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * gpu::kFramesInFlight};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = gpu::kFramesInFlight;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(ctx_.device(), &poolInfo, nullptr, &descriptorPool_));

    VkDescriptorSetLayout layouts[gpu::kFramesInFlight];
    for (auto& layout : layouts) layout = frameSetLayout_;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = gpu::kFramesInFlight;
    allocInfo.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(ctx_.device(), &allocInfo, frameSets_));

    // Pipeline layout: set 0 bindless, set 1 frame data, push constant viewport.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.size = sizeof(Vec2);

    VkDescriptorSetLayout setLayouts[] = {bindless_.layout(), frameSetLayout_};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 2;
    pipelineLayoutInfo.pSetLayouts = setLayouts;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(
        vkCreatePipelineLayout(ctx_.device(), &pipelineLayoutInfo, nullptr, &pipelineLayout_));

    // Pipeline: 4-vertex strip, no vertex input, alpha blending, dynamic
    // rendering into the swapchain format.
    VkShaderModule vert = createModule(ctx_.device(), quad2d_vert_spv, quad2d_vert_spv_words);
    VkShaderModule frag = createModule(ctx_.device(), quad2d_frag_spv, quad2d_frag_spv_words);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
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
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = pipelineLayout_;
    VK_CHECK(vkCreateGraphicsPipelines(ctx_.device(), ctx_.pipelineCache(), 1, &pipelineInfo, nullptr,
                                       &pipeline_));

    vkDestroyShaderModule(ctx_.device(), vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), frag, nullptr);
}

Renderer2D::~Renderer2D() {
    for (auto& buf : quadBuffers_)
        if (buf.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(ctx_.allocator(), buf.buffer, buf.allocation);
    for (auto& buf : clipBuffers_)
        if (buf.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(ctx_.allocator(), buf.buffer, buf.allocation);
    vkDestroyPipeline(ctx_.device(), pipeline_, nullptr);
    vkDestroyPipelineLayout(ctx_.device(), pipelineLayout_, nullptr);
    vkDestroyDescriptorPool(ctx_.device(), descriptorPool_, nullptr);
    vkDestroyDescriptorSetLayout(ctx_.device(), frameSetLayout_, nullptr);
}

void Renderer2D::ensureCapacity(MappedBuffer& buf, size_t bytes, uint32_t slot,
                                VkBufferUsageFlags usage, gpu::FrameRing& frames) {
    if (buf.capacity >= bytes) return;
    size_t newCapacity = buf.capacity == 0 ? 64 * 1024 : buf.capacity;
    while (newCapacity < bytes) newCapacity *= 2;

    if (buf.buffer != VK_NULL_HANDLE) {
        VkBuffer oldBuffer = buf.buffer;
        VmaAllocation oldAllocation = buf.allocation;
        VmaAllocator allocator = ctx_.allocator();
        frames.defer([allocator, oldBuffer, oldAllocation] {
            vmaDestroyBuffer(allocator, oldBuffer, oldAllocation);
        });
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = newCapacity;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
    allocCreate.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
    allocCreate.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    VmaAllocationInfo allocInfo{};
    VK_CHECK(vmaCreateBuffer(ctx_.allocator(), &bufferInfo, &allocCreate, &buf.buffer,
                             &buf.allocation, &allocInfo));
    buf.mapped = allocInfo.pMappedData;
    buf.capacity = newCapacity;
    descriptorsDirty_[slot] = true;
}

void Renderer2D::updateDescriptors(uint32_t slot) {
    VkDescriptorBufferInfo bufferInfos[2]{};
    bufferInfos[0] = {quadBuffers_[slot].buffer, 0, VK_WHOLE_SIZE};
    bufferInfos[1] = {clipBuffers_[slot].buffer, 0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = frameSets_[slot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfos[i];
    }
    vkUpdateDescriptorSets(ctx_.device(), 2, writes, 0, nullptr);
    descriptorsDirty_[slot] = false;
}

void Renderer2D::flush(VkCommandBuffer cmd, uint32_t slot, const CanvasData& data,
                       gpu::FrameRing& frames) {
    if (data.quads.empty()) return;

    const size_t quadBytes = data.quads.size() * sizeof(Quad2D);
    const size_t clipBytes = data.clips.size() * sizeof(Vec4);
    ensureCapacity(quadBuffers_[slot], quadBytes, slot, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   frames);
    ensureCapacity(clipBuffers_[slot], clipBytes, slot, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   frames);
    std::memcpy(quadBuffers_[slot].mapped, data.quads.data(), quadBytes);
    std::memcpy(clipBuffers_[slot].mapped, data.clips.data(), clipBytes);
    if (descriptorsDirty_[slot]) updateDescriptors(slot);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    const VkDescriptorSet sets[] = {bindless_.set(), frameSets_[slot]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0,
                            nullptr);

    VkViewport viewport{0.0f, 0.0f, data.viewport.x, data.viewport.y, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{{0, 0},
                     {static_cast<uint32_t>(data.viewport.x),
                      static_cast<uint32_t>(data.viewport.y)}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(Vec2), &data.viewport);

    vkCmdDraw(cmd, 4, static_cast<uint32_t>(data.quads.size()), 0, 0);
}

} // namespace rendy::detail
