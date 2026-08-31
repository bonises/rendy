#pragma once

// Frames-in-flight ring: command buffers, acquire/present, CPU-GPU sync via
// one timeline semaphore, and a deletion queue for safe resource retirement.

#include "gpu/context.hpp"
#include "gpu/swapchain.hpp"

#include <array>
#include <functional>
#include <vector>

namespace rendy::gpu {

inline constexpr uint32_t kFramesInFlight = 2;

class FrameRing {
public:
    FrameRing(Context& ctx, Swapchain& swapchain);
    ~FrameRing();

    FrameRing(const FrameRing&) = delete;
    FrameRing& operator=(const FrameRing&) = delete;

    struct FrameInfo {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint32_t imageIndex = 0;
        bool ok = false; ///< false when the surface is unusable (minimized)
    };

    /// Waits for this slot's previous work, acquires a swapchain image
    /// (recreating the swapchain if needed), and begins the command buffer.
    FrameInfo begin();

    /// Ends the command buffer, submits, presents.
    void end();

    /// Run `fn` once the GPU is done with the current frame slot.
    void defer(std::function<void()> fn);

    /// Index of the current frame slot (0..kFramesInFlight-1), for per-frame
    /// resources owned by other systems.
    uint32_t slot() const { return static_cast<uint32_t>(frameCounter_ % kFramesInFlight); }
    uint64_t frameCounter() const { return frameCounter_; }

private:
    void flushDeletions(size_t slotIndex);

    Context& ctx_;
    Swapchain& swapchain_;

    struct PerFrame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
        std::vector<std::function<void()>> deletions;
    };
    std::array<PerFrame, kFramesInFlight> frames_;
    // One render-finished semaphore per swapchain image (present waits on it).
    std::vector<VkSemaphore> renderFinished_;

    VkSemaphore timeline_ = VK_NULL_HANDLE;
    uint64_t frameCounter_ = 0; // monotonically increasing; timeline signals counter+1
    uint32_t imageIndex_ = 0;
    bool frameActive_ = false;
};

} // namespace rendy::gpu
