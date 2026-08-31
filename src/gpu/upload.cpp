#include "gpu/upload.hpp"

#include <cstring>

namespace rendy::gpu {

Uploader::Uploader(Context& ctx) : ctx_(ctx) {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = ctx_.graphicsFamily();
    VK_CHECK(vkCreateCommandPool(ctx_.device(), &poolInfo, nullptr, &pool_));

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx_.device(), &allocInfo, &cmd_));

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VK_CHECK(vkCreateFence(ctx_.device(), &fenceInfo, nullptr, &fence_));
}

Uploader::~Uploader() {
    vkDestroyFence(ctx_.device(), fence_, nullptr);
    vkDestroyCommandPool(ctx_.device(), pool_, nullptr);
}

void Uploader::submit(const void* data, size_t size,
                      const std::function<void(VkCommandBuffer, VkBuffer)>& record) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
    allocCreate.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocInfo{};
    VK_CHECK(vmaCreateBuffer(ctx_.allocator(), &bufferInfo, &allocCreate, &staging, &allocation,
                             &allocInfo));
    std::memcpy(allocInfo.pMappedData, data, size);

    vkResetCommandPool(ctx_.device(), pool_, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd_, &beginInfo));
    record(cmd_, staging);
    VK_CHECK(vkEndCommandBuffer(cmd_));

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd_;
    VK_CHECK(vkQueueSubmit(ctx_.graphicsQueue(), 1, &submitInfo, fence_));
    VK_CHECK(vkWaitForFences(ctx_.device(), 1, &fence_, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(ctx_.device(), 1, &fence_));

    vmaDestroyBuffer(ctx_.allocator(), staging, allocation);
}

void Uploader::uploadImage(VkImage image, const void* pixels, size_t size, uint32_t width,
                           uint32_t height) {
    submit(pixels, size, [&](VkCommandBuffer cmd, VkBuffer staging) {
        imageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                     0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);

        imageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
}

} // namespace rendy::gpu
