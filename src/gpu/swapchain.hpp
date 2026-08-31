#pragma once

// Swapchain wrapper. Recreation on resize is driven by FrameRing.

#include "gpu/context.hpp"

#include <vector>

namespace rendy::gpu {

class Swapchain {
public:
    Swapchain(Context& ctx, VkSurfaceKHR surface, bool vsync);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    /// (Re)build for the current surface size. Returns false if the surface
    /// is currently zero-sized (minimized) — caller should skip the frame.
    bool recreate();

    VkSwapchainKHR handle() const { return swapchain_; }
    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }
    uint32_t imageCount() const { return static_cast<uint32_t>(images_.size()); }
    VkImage image(uint32_t i) const { return images_[i]; }
    VkImageView imageView(uint32_t i) const { return imageViews_[i]; }

private:
    void destroyViews();

    Context& ctx_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_SRGB;
    VkColorSpaceKHR colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{};
    bool vsync_ = true;
    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;
};

} // namespace rendy::gpu
