#pragma once

// GPU half of the 2D batch: per-frame mapped buffers, one pipeline, one draw.

#include "canvas/canvas_data.hpp"
#include "gpu/bindless.hpp"
#include "gpu/context.hpp"
#include "gpu/frame.hpp"

namespace rendy::detail {

class Renderer2D {
public:
    Renderer2D(gpu::Context& ctx, gpu::BindlessTable& bindless, VkFormat colorFormat);
    ~Renderer2D();

    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;

    /// Upload the batch for this frame slot and record the draw. Must be
    /// called inside an active dynamic rendering pass.
    void flush(VkCommandBuffer cmd, uint32_t slot, const CanvasData& data,
               gpu::FrameRing& frames);

private:
    struct MappedBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
        size_t capacity = 0;
    };

    void ensureCapacity(MappedBuffer& buf, size_t bytes, uint32_t slot, VkBufferUsageFlags usage,
                        gpu::FrameRing& frames);
    void updateDescriptors(uint32_t slot);

    gpu::Context& ctx_;
    gpu::BindlessTable& bindless_;

    VkDescriptorSetLayout frameSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet frameSets_[gpu::kFramesInFlight]{};
    bool descriptorsDirty_[gpu::kFramesInFlight]{};

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    MappedBuffer quadBuffers_[gpu::kFramesInFlight];
    MappedBuffer clipBuffers_[gpu::kFramesInFlight];
};

} // namespace rendy::detail
