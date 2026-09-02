#include "scene/mesh_store.hpp"

#include <cstring>

namespace rendy::detail {
namespace {

constexpr size_t kInitialVertexBytes = 4 * 1024 * 1024;
constexpr size_t kInitialIndexBytes = 1024 * 1024;

void createBuffer(gpu::Context& ctx, size_t bytes, VkBufferUsageFlags usage, VkBuffer* buffer,
                  VmaAllocation* allocation) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &info, &allocCreate, buffer, allocation, nullptr));
}

} // namespace

MeshStore::MeshStore(gpu::Context& ctx, gpu::Uploader& uploader)
    : ctx_(ctx), uploader_(uploader) {
    vertexCapacity_ = kInitialVertexBytes;
    indexCapacity_ = kInitialIndexBytes;
    morphCapacity_ = sizeof(MorphDelta); // grown on first morphed mesh
    createBuffer(ctx_, vertexCapacity_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vertexBuffer_,
                 &vertexAllocation_);
    createBuffer(ctx_, indexCapacity_, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &indexBuffer_,
                 &indexAllocation_);
    createBuffer(ctx_, morphCapacity_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &morphBuffer_,
                 &morphAllocation_);
}

MeshStore::~MeshStore() {
    vmaDestroyBuffer(ctx_.allocator(), vertexBuffer_, vertexAllocation_);
    vmaDestroyBuffer(ctx_.allocator(), indexBuffer_, indexAllocation_);
    vmaDestroyBuffer(ctx_.allocator(), morphBuffer_, morphAllocation_);
}

void MeshStore::ensureRoom(size_t vertexBytes, size_t indexBytes) {
    const size_t neededVertex = vertexBytes;
    const size_t neededIndex = indexBytes;
    if (neededVertex <= vertexCapacity_ && neededIndex <= indexCapacity_) return;

    size_t newVertexCapacity = vertexCapacity_;
    while (newVertexCapacity < neededVertex) newVertexCapacity *= 2;
    size_t newIndexCapacity = indexCapacity_;
    while (newIndexCapacity < neededIndex) newIndexCapacity *= 2;

    // Grow by copy on the GPU. Blocking is fine: mesh creation is load-time.
    // NOTE: destroying the old buffers right after is safe ONLY because the
    // Uploader submits on the same graphics queue and waits on a fence —
    // revisit if a dedicated transfer queue is introduced.
    VkBuffer newVertex;
    VmaAllocation newVertexAlloc;
    VkBuffer newIndex;
    VmaAllocation newIndexAlloc;
    createBuffer(ctx_, newVertexCapacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &newVertex,
                 &newVertexAlloc);
    createBuffer(ctx_, newIndexCapacity, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &newIndex,
                 &newIndexAlloc);

    // ensureRoom runs AFTER the new mesh's blocks were allocated, so end()
    // can already include the (still unwritten) new region — clamp the copy
    // to the old buffer, which holds all previously written data.
    const size_t copyVertexBytes =
        std::min(vertexAlloc_.end() * sizeof(Vertex), vertexCapacity_);
    const size_t copyIndexBytes =
        std::min(indexAlloc_.end() * sizeof(uint32_t), indexCapacity_);
    const uint8_t dummy = 0;
    uploader_.submit(&dummy, 1, [&](VkCommandBuffer cmd, VkBuffer) {
        if (copyVertexBytes > 0) {
            VkBufferCopy copy{0, 0, copyVertexBytes};
            vkCmdCopyBuffer(cmd, vertexBuffer_, newVertex, 1, &copy);
        }
        if (copyIndexBytes > 0) {
            VkBufferCopy copy{0, 0, copyIndexBytes};
            vkCmdCopyBuffer(cmd, indexBuffer_, newIndex, 1, &copy);
        }
    });

    vmaDestroyBuffer(ctx_.allocator(), vertexBuffer_, vertexAllocation_);
    vmaDestroyBuffer(ctx_.allocator(), indexBuffer_, indexAllocation_);
    vertexBuffer_ = newVertex;
    vertexAllocation_ = newVertexAlloc;
    indexBuffer_ = newIndex;
    indexAllocation_ = newIndexAlloc;
    vertexCapacity_ = newVertexCapacity;
    indexCapacity_ = newIndexCapacity;
}

MeshHandle MeshStore::add(const MeshData& data) {
    const size_t vertexBytes = data.vertices.size() * sizeof(Vertex);
    const size_t indexBytes = data.indices.size() * sizeof(uint32_t);
    // Reuse freed space when a fitting hole exists, else bump-allocate.
    const uint32_t vertexOffset =
        vertexAlloc_.allocate(static_cast<uint32_t>(data.vertices.size()));
    const uint32_t indexOffset =
        indexAlloc_.allocate(static_cast<uint32_t>(data.indices.size()));
    ensureRoom(vertexAlloc_.end() * sizeof(Vertex), indexAlloc_.end() * sizeof(uint32_t));

    uploader_.submit(data.vertices.data(), vertexBytes,
                     [&](VkCommandBuffer cmd, VkBuffer staging) {
                         VkBufferCopy copy{0, vertexOffset * sizeof(Vertex), vertexBytes};
                         vkCmdCopyBuffer(cmd, staging, vertexBuffer_, 1, &copy);
                     });
    uploader_.submit(data.indices.data(), indexBytes,
                     [&](VkCommandBuffer cmd, VkBuffer staging) {
                         VkBufferCopy copy{0, indexOffset * sizeof(uint32_t), indexBytes};
                         vkCmdCopyBuffer(cmd, staging, indexBuffer_, 1, &copy);
                     });

    // Morph target deltas → the shared delta SSBO.
    uint32_t morphDeltaBase = UINT32_MAX;
    if (!data.morphTargets.empty()) {
        const size_t vertexCount = data.vertices.size();
        std::vector<MorphDelta> deltas;
        deltas.reserve(data.morphTargets.size() * vertexCount);
        for (const MorphTarget& target : data.morphTargets) {
            for (size_t v = 0; v < vertexCount; ++v) {
                MorphDelta delta{};
                if (v < target.positionDeltas.size())
                    delta.position = Vec4{target.positionDeltas[v], 0.0f};
                if (v < target.normalDeltas.size())
                    delta.normal = Vec4{target.normalDeltas[v], 0.0f};
                deltas.push_back(delta);
            }
        }
        const size_t deltaBytes = deltas.size() * sizeof(MorphDelta);
        const size_t needed = (morphEntryCount_ + deltas.size()) * sizeof(MorphDelta);
        if (needed > morphCapacity_) {
            size_t newCapacity = std::max<size_t>(morphCapacity_, 64 * 1024);
            while (newCapacity < needed) newCapacity *= 2;
            VkBuffer newBuffer;
            VmaAllocation newAllocation;
            createBuffer(ctx_, newCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &newBuffer,
                         &newAllocation);
            const uint8_t dummy = 0;
            uploader_.submit(&dummy, 1, [&](VkCommandBuffer cmd, VkBuffer) {
                if (morphEntryCount_ > 0) {
                    VkBufferCopy copy{0, 0, morphEntryCount_ * sizeof(MorphDelta)};
                    vkCmdCopyBuffer(cmd, morphBuffer_, newBuffer, 1, &copy);
                }
            });
            vmaDestroyBuffer(ctx_.allocator(), morphBuffer_, morphAllocation_);
            morphBuffer_ = newBuffer;
            morphAllocation_ = newAllocation;
            morphCapacity_ = newCapacity;
        }
        uploader_.submit(deltas.data(), deltaBytes, [&](VkCommandBuffer cmd, VkBuffer staging) {
            VkBufferCopy copy{0, morphEntryCount_ * sizeof(MorphDelta), deltaBytes};
            vkCmdCopyBuffer(cmd, staging, morphBuffer_, 1, &copy);
        });
        morphDeltaBase = morphEntryCount_;
        morphEntryCount_ += static_cast<uint32_t>(deltas.size());
    }

    MeshRange range;
    range.morphDeltaBase = morphDeltaBase;
    range.morphTargetCount = static_cast<uint32_t>(data.morphTargets.size());
    range.vertexCount = static_cast<uint32_t>(data.vertices.size());
    range.firstIndex = indexOffset;
    range.indexCount = static_cast<uint32_t>(data.indices.size());
    range.vertexOffset = static_cast<int32_t>(vertexOffset);
    range.boundsCenter = data.boundsCenter;
    range.boundsRadius = data.boundsRadius;
    if (range.boundsRadius <= 0.0f) {
        for (const Vertex& vertex : data.vertices)
            range.boundsRadius =
                std::max(range.boundsRadius, glm::length(vertex.position - range.boundsCenter));
    }

    if (!freeRangeIds_.empty()) {
        const uint32_t id = freeRangeIds_.back();
        freeRangeIds_.pop_back();
        ranges_[id] = range;
        return MeshHandle{id, generations_[id]};
    }
    ranges_.push_back(range);
    generations_.push_back(0);
    return MeshHandle{static_cast<uint32_t>(ranges_.size() - 1), 0};
}

void MeshStore::destroy(MeshHandle handle) {
    if (!valid(handle)) return;
    MeshRange& range = ranges_[handle.id];
    vertexAlloc_.free(static_cast<uint32_t>(range.vertexOffset), range.vertexCount);
    indexAlloc_.free(range.firstIndex, range.indexCount);
    // Morph deltas stay allocated (append-only store) — acceptable leak for
    // the rare destroy-a-morphed-mesh case.
    range = {};
    range.alive = false;
    ++generations_[handle.id]; // a reused slot never matches the old handle
    freeRangeIds_.push_back(handle.id);
}

} // namespace rendy::detail
