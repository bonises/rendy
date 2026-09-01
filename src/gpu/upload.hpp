#pragma once

// Blocking upload path for load-time assets: staging buffer + one-shot
// command buffer + wait. Fine for asset loads; per-frame data uses the
// mapped per-frame buffers instead.

#include "gpu/context.hpp"

#include <cstddef>
#include <functional>

namespace rendy::gpu {

class Uploader {
public:
    explicit Uploader(Context& ctx);
    ~Uploader();

    Uploader(const Uploader&) = delete;
    Uploader& operator=(const Uploader&) = delete;

    /// Copies `size` bytes into a fresh image via staging. `record` receives
    /// the command buffer and the staging buffer to record copies/transitions.
    void submit(const void* data, size_t size,
                const std::function<void(VkCommandBuffer, VkBuffer)>& record);

    /// Standard image upload: staging → transition → copy → SHADER_READ_ONLY.
    /// With mipLevels > 1, generates the chain with blits.
    void uploadImage(VkImage image, const void* pixels, size_t size, uint32_t width,
                     uint32_t height, uint32_t mipLevels = 1);

private:
    Context& ctx_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};

} // namespace rendy::gpu
