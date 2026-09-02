#pragma once

// Pure swapchain selection logic, split out of Swapchain for CPU unit
// testing (no device needed).

// volk directly (not vk_common.hpp) — keeps the header includable from the
// CPU test suite without dragging VMA in.
#include <volk.h>

#include <optional>
#include <vector>

namespace rendy::gpu {

/// Picks a surface format the render + screenshot paths actually support:
/// 8-bit sRGB, 4 bytes/pixel (BGRA8 preferred, RGBA8 accepted). Anything
/// else — UNORM, 10-bit, HDR — would silently break the sRGB-encode-on-
/// write contract, so it's rejected rather than half-supported.
inline std::optional<VkSurfaceFormatKHR>
chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (VkFormat wanted : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB})
        for (const VkSurfaceFormatKHR& format : formats)
            if (format.format == wanted &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return format;
    return std::nullopt;
}

/// Usage flags for the swapchain images. Only COLOR_ATTACHMENT is
/// guaranteed for presentable images — the transfer bits (TRANSFER_SRC
/// enables screenshot readback) are requested only when the surface
/// supports them.
inline VkImageUsageFlags chooseSwapchainUsage(VkImageUsageFlags supported) {
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    usage |= supported & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    return usage;
}

} // namespace rendy::gpu
