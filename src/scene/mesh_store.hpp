#pragma once

// GPU mesh storage: all static mesh data lives in two big device-local
// buffers (one vertex, one index), bound once per scene pass. Vertex/index
// space is recycled through a free-list allocator; morph deltas stay
// append-only (rarely destroyed).

#include "gpu/context.hpp"
#include "gpu/upload.hpp"
#include "scene/block_allocator.hpp"
#include "rendy/scene/mesh.hpp"

#include <vector>

namespace rendy::detail {

struct MeshRange {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t vertexOffset = 0;
    Vec3 boundsCenter{0.0f};
    float boundsRadius = 0.0f;
    // Morph targets: entry offset into the delta buffer (per target, per
    // vertex), UINT32_MAX = none.
    uint32_t morphDeltaBase = UINT32_MAX;
    uint32_t morphTargetCount = 0;
    uint32_t vertexCount = 0;
    bool alive = true; ///< false after destroy(); handle slot reused later
};

// GPU layout of one morph delta (std430).
struct MorphDelta {
    Vec4 position; // xyz used
    Vec4 normal;
};

class MeshStore {
public:
    MeshStore(gpu::Context& ctx, gpu::Uploader& uploader);
    ~MeshStore();

    MeshStore(const MeshStore&) = delete;
    MeshStore& operator=(const MeshStore&) = delete;

    MeshHandle add(const MeshData& data);
    /// Frees the mesh's vertex/index space for reuse and retires the handle
    /// (its id slot is recycled by a later add — don't keep dead handles).
    /// GPU-safe mid-flight: the space is only rewritten by later uploads on
    /// the same queue.
    void destroy(MeshHandle handle);
    [[nodiscard]] bool valid(MeshHandle handle) const {
        return handle.id < ranges_.size() && ranges_[handle.id].alive;
    }
    [[nodiscard]] const MeshRange& range(MeshHandle handle) const {
        return ranges_[handle.id];
    }

    VkBuffer vertexBuffer() const { return vertexBuffer_; }
    VkBuffer indexBuffer() const { return indexBuffer_; }
    /// Valid buffer even when no mesh has morphs (1-entry dummy).
    VkBuffer morphDeltaBuffer() const { return morphBuffer_; }

private:
    void ensureRoom(size_t vertexBytes, size_t indexBytes);

    gpu::Context& ctx_;
    gpu::Uploader& uploader_;
    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation_ = VK_NULL_HANDLE;
    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation indexAllocation_ = VK_NULL_HANDLE;
    VkBuffer morphBuffer_ = VK_NULL_HANDLE;
    VmaAllocation morphAllocation_ = VK_NULL_HANDLE;
    size_t vertexCapacity_ = 0;
    size_t indexCapacity_ = 0;
    size_t morphCapacity_ = 0;
    BlockAllocator vertexAlloc_; // units: vertices
    BlockAllocator indexAlloc_;  // units: indices
    uint32_t morphEntryCount_ = 0;
    std::vector<MeshRange> ranges_;
    std::vector<uint32_t> freeRangeIds_;
};

} // namespace rendy::detail
