#pragma once

// Swapchain wrapper. Recreation on resize is driven by FrameRing.

#include "gpu/context.hpp"

#include <cstdint>
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

    /// The window told us its pixel size changed. X11/Wayland don't
    /// reliably return VK_ERROR_OUT_OF_DATE on resize (hidden windows on
    /// X11 never do), so FrameRing also recreates when this is set.
    void markStale() { stale_ = true; }
    bool stale() const { return stale_; }

    /// Screenshot readback possible? (Needs surface TRANSFER_SRC support
    /// and one of the two 8-bit sRGB formats — both optional in Vulkan.)
    bool captureSupported() const { return captureSupported_; }

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
    bool formatCapturable_ = false; ///< 8-bit sRGB, 4 bytes/pixel
    bool captureSupported_ = false; ///< formatCapturable_ && TRANSFER_SRC usage
    VkColorSpaceKHR colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{};
    bool vsync_ = true;
    bool stale_ = false; ///< window resized; recreate before next acquire
    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;
};

} // namespace rendy::gpu
