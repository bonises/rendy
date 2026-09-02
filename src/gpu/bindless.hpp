#pragma once

// The global bindless descriptor set (set 0): a large partially-bound array
// of combined image samplers. Every texture in rendy gets a stable u32 index
// here at creation, so texture changes never break draw batches.

#include "gpu/context.hpp"

#include <cstdint>
#include <vector>

namespace rendy::gpu {

inline constexpr uint32_t kMaxBindlessTextures = 4096;

class BindlessTable {
public:
    explicit BindlessTable(Context& ctx);
    ~BindlessTable();

    BindlessTable(const BindlessTable&) = delete;
    BindlessTable& operator=(const BindlessTable&) = delete;

    /// Registers a texture and returns its stable index. Index 0 is reserved
    /// for the built-in 1x1 white texture (registered by TexturePool).
    uint32_t add(VkImageView view, VkSampler sampler);
    /// Frees an index for reuse. Caller guarantees the GPU no longer samples
    /// it (defer via FrameRing).
    void remove(uint32_t index);

    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet set() const { return set_; }

private:
    void write(uint32_t index, VkImageView view, VkSampler sampler);

    Context& ctx_;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    uint32_t nextIndex_ = 0;
    std::vector<uint32_t> freeList_;
};

} // namespace rendy::gpu
