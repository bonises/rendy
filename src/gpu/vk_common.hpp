#pragma once

// Shared internal Vulkan includes + helpers. Never included from public headers.

#include <volk.h>

#include <vk_mem_alloc.h>

#include "rendy/core/log.hpp"

#include <cstdlib>

// Vulkan calls that must succeed. Failure here is a bug or a lost device;
// neither is recoverable in v1, so log and abort.
#define VK_CHECK(expr)                                                                   \
    do {                                                                                 \
        const VkResult vk_check_result = (expr);                                         \
        if (vk_check_result != VK_SUCCESS) {                                             \
            ::rendy::log::error("{} failed: VkResult {} ({}:{})", #expr,                 \
                                static_cast<int>(vk_check_result), __FILE__, __LINE__);  \
            std::abort();                                                                \
        }                                                                                \
    } while (0)

namespace rendy::gpu {

// Full-subresource image layout transition via synchronization2.
inline void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                         VkImageLayout newLayout, VkPipelineStageFlags2 srcStage,
                         VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                         VkAccessFlags2 dstAccess,
                         VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace rendy::gpu
