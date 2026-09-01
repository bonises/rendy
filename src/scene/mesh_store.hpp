#pragma once

// GPU mesh storage: all static mesh data lives in two big device-local
// buffers (one vertex, one index), bound once per scene pass. v1 allocates
// append-only; destroyed meshes leave holes until a future compactor.

#include "gpu/context.hpp"
#include "gpu/upload.hpp"
#include "rendy/scene/mesh.hpp"

#include <vector>

namespace rendy::detail {

struct MeshRange {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t vertexOffset = 0;
    Vec3 boundsCenter{0.0f};
    float boundsRadius = 0.0f;
};

class MeshStore {
public:
    MeshStore(gpu::Context& ctx, gpu::Uploader& uploader);
    ~MeshStore();

    MeshStore(const MeshStore&) = delete;
    MeshStore& operator=(const MeshStore&) = delete;

    MeshHandle add(const MeshData& data);
    [[nodiscard]] const MeshRange& range(MeshHandle handle) const {
        return ranges_[handle.id];
    }

    VkBuffer vertexBuffer() const { return vertexBuffer_; }
    VkBuffer indexBuffer() const { return indexBuffer_; }

private:
    void ensureRoom(size_t vertexBytes, size_t indexBytes);

    gpu::Context& ctx_;
    gpu::Uploader& uploader_;
    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation_ = VK_NULL_HANDLE;
    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation indexAllocation_ = VK_NULL_HANDLE;
    size_t vertexCapacity_ = 0;
    size_t indexCapacity_ = 0;
    uint32_t vertexCount_ = 0;
    uint32_t indexCount_ = 0;
    std::vector<MeshRange> ranges_;
};

} // namespace rendy::detail
