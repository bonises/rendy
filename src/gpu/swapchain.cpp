#include "gpu/swapchain.hpp"

#include <algorithm>

namespace rendy::gpu {

Swapchain::Swapchain(Context& ctx, VkSurfaceKHR surface, bool vsync)
    : ctx_(ctx), surface_(surface), vsync_(vsync) {
    // Pick format once: prefer BGRA8 sRGB, else first available.
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physicalDevice(), surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physicalDevice(), surface_, &formatCount,
                                         formats.data());
    format_ = formats[0].format;
    colorSpace_ = formats[0].colorSpace;
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            format_ = f.format;
            colorSpace_ = f.colorSpace;
            break;
        }
    }
    recreate();
}

Swapchain::~Swapchain() {
    destroyViews();
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(ctx_.device(), swapchain_, nullptr);
    if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(ctx_.instance(), surface_, nullptr);
}

void Swapchain::destroyViews() {
    for (VkImageView view : imageViews_) vkDestroyImageView(ctx_.device(), view, nullptr);
    imageViews_.clear();
    images_.clear();
}

bool Swapchain::recreate() {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physicalDevice(), surface_, &caps));

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFF) extent = {1280, 720}; // surface lets us choose
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height =
        std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    if (extent.width == 0 || extent.height == 0) return false;

    // Present mode: FIFO is always available; mailbox when vsync is off (low
    // latency without tearing), immediate as last resort.
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_.physicalDevice(), surface_, &modeCount,
                                                  nullptr);
        std::vector<VkPresentModeKHR> modes(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_.physicalDevice(), surface_, &modeCount,
                                                  modes.data());
        for (VkPresentModeKHR m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) presentMode = m;
        if (presentMode == VK_PRESENT_MODE_FIFO_KHR)
            for (VkPresentModeKHR m : modes)
                if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) presentMode = m;
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainKHR oldSwapchain = swapchain_;

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = imageCount;
    info.imageFormat = format_;
    info.imageColorSpace = colorSpace_;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    // TRANSFER_SRC enables screenshot readback (App::requestScreenshot).
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;

    VK_CHECK(vkCreateSwapchainKHR(ctx_.device(), &info, nullptr, &swapchain_));
    if (oldSwapchain != VK_NULL_HANDLE) {
        ctx_.waitIdle();
        vkDestroySwapchainKHR(ctx_.device(), oldSwapchain, nullptr);
    }
    destroyViews();
    extent_ = extent;

    uint32_t actualCount = 0;
    vkGetSwapchainImagesKHR(ctx_.device(), swapchain_, &actualCount, nullptr);
    images_.resize(actualCount);
    vkGetSwapchainImagesKHR(ctx_.device(), swapchain_, &actualCount, images_.data());

    imageViews_.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format_;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx_.device(), &viewInfo, nullptr, &imageViews_[i]));
    }
    return true;
}

} // namespace rendy::gpu
