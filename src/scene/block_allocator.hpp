#pragma once

// Free-list block allocator for the mesh store's shared buffers: first-fit
// over freed [offset, count) blocks with neighbor coalescing, falling back
// to bump allocation at the high-water mark. Unit-testable (no GPU).

#include <cstdint>
#include <vector>

namespace rendy::detail {

class BlockAllocator {
public:
    /// Allocates `count` units: first free block that fits (splitting it),
    /// else bump-allocated at end() (growing it). count 0 returns end().
    uint32_t allocate(uint32_t count) {
        for (size_t i = 0; i < free_.size(); ++i) {
            if (free_[i].count < count) continue;
            const uint32_t offset = free_[i].offset;
            if (free_[i].count == count) {
                free_.erase(free_.begin() + static_cast<long>(i));
            } else {
                free_[i].offset += count;
                free_[i].count -= count;
            }
            return offset;
        }
        const uint32_t offset = end_;
        end_ += count;
        return offset;
    }

    /// Returns a block; coalesces with adjacent free blocks, and shrinks
    /// end() when the merged block touches it.
    void free(uint32_t offset, uint32_t count) {
        if (count == 0) return;
        // Insert sorted by offset.
        size_t at = 0;
        while (at < free_.size() && free_[at].offset < offset) ++at;
        free_.insert(free_.begin() + static_cast<long>(at), {offset, count});
        // Coalesce with the next block, then the previous one.
        if (at + 1 < free_.size() &&
            free_[at].offset + free_[at].count == free_[at + 1].offset) {
            free_[at].count += free_[at + 1].count;
            free_.erase(free_.begin() + static_cast<long>(at) + 1);
        }
        if (at > 0 && free_[at - 1].offset + free_[at - 1].count == free_[at].offset) {
            free_[at - 1].count += free_[at].count;
            free_.erase(free_.begin() + static_cast<long>(at));
            --at;
        }
        // A tail block at the high-water mark returns to the bump region.
        if (!free_.empty() && free_.back().offset + free_.back().count == end_) {
            end_ = free_.back().offset;
            free_.pop_back();
        }
    }

    /// High-water mark: the caller's backing store must hold this many units.
    [[nodiscard]] uint32_t end() const { return end_; }
    /// Units currently sitting in the free list (fragmentation metric).
    [[nodiscard]] uint32_t freeUnits() const {
        uint32_t total = 0;
        for (const Block& block : free_) total += block.count;
        return total;
    }

private:
    struct Block {
        uint32_t offset;
        uint32_t count;
    };
    std::vector<Block> free_; // sorted by offset, no adjacent blocks
    uint32_t end_ = 0;
};

} // namespace rendy::detail
