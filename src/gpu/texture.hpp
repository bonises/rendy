#pragma once

// Internal texture storage: VkImage + view + sampler per texture, registered
// in the bindless table. Index 0 is a 1x1 white texture.

#include "gpu/bindless.hpp"
#include "gpu/context.hpp"
#include "gpu/upload.hpp"
#include "rendy/core/result.hpp"
#include "rendy/gpu/texture.hpp"

#include <string>
#include <unordered_map>

namespace rendy::gpu {

class TexturePool {
public:
    TexturePool(Context& ctx, BindlessTable& bindless, Uploader& uploader);
    ~TexturePool();

    TexturePool(const TexturePool&) = delete;
    TexturePool& operator=(const TexturePool&) = delete;

    /// RGBA8 pixels, tightly packed.
    Result<TextureRef> createFromPixels(const void* rgba, IVec2 size,
                                        const TextureOptions& options);
    /// Single-channel (R8) pixels — used by the glyph atlas.
    Result<TextureRef> createR8(const void* pixels, IVec2 size);
    Result<TextureRef> loadFromFile(const std::string& path, const TextureOptions& options);

    /// Re-upload pixels into an existing texture (must match size/format).
    void update(TextureRef ref, const void* pixels, size_t bytes);

    /// Safe to call while frames are in flight; actual destruction is the
    /// caller's job to defer (App defers via FrameRing).
    void destroy(TextureRef ref);

private:
    struct Entry {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        IVec2 size{0, 0};
        VkFormat format = VK_FORMAT_UNDEFINED;
    };

    Result<TextureRef> create(const void* pixels, IVec2 size, VkFormat format,
                              uint32_t bytesPerPixel, const TextureOptions& options);
    void destroyEntry(Entry& entry);

    Context& ctx_;
    BindlessTable& bindless_;
    Uploader& uploader_;
    std::unordered_map<uint32_t, Entry> entries_; // keyed by bindless index
};

} // namespace rendy::gpu
