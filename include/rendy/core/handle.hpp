#pragma once

/// \file handle.hpp
/// Generational handles. rendy's public API never hands out raw pointers to
/// GPU-side objects; it hands out Handle<Tag> values that stay cheap to copy
/// and detect use-after-destroy via a generation counter.

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace rendy {

/// A typed 64-bit handle: 32-bit slot index + 32-bit generation.
/// Tag is a phantom type (e.g. `struct TextureTag`) so handles of different
/// resources don't mix.
template <typename Tag>
struct Handle {
    uint32_t index = 0;
    uint32_t generation = 0; // 0 = invalid

    [[nodiscard]] bool valid() const { return generation != 0; }
    explicit operator bool() const { return valid(); }

    friend bool operator==(Handle, Handle) = default;
};

/// Slot storage with a free list backing Handle<Tag>. Destroyed slots bump
/// their generation, so stale handles resolve to nullptr instead of aliasing
/// a recycled slot.
template <typename T, typename Tag>
class HandlePool {
public:
    using HandleType = Handle<Tag>;

    template <typename... Args>
    HandleType create(Args&&... args) {
        uint32_t index;
        if (!freeList_.empty()) {
            index = freeList_.back();
            freeList_.pop_back();
            slots_[index].value = T{std::forward<Args>(args)...};
            slots_[index].alive = true;
        } else {
            index = static_cast<uint32_t>(slots_.size());
            slots_.push_back(Slot{T{std::forward<Args>(args)...}, 1, true});
        }
        return HandleType{index, slots_[index].generation};
    }

    /// nullptr if the handle is invalid, stale, or destroyed.
    [[nodiscard]] T* get(HandleType handle) {
        if (!handle.valid() || handle.index >= slots_.size()) return nullptr;
        Slot& slot = slots_[handle.index];
        if (!slot.alive || slot.generation != handle.generation) return nullptr;
        return &slot.value;
    }
    [[nodiscard]] const T* get(HandleType handle) const {
        return const_cast<HandlePool*>(this)->get(handle);
    }

    /// Returns true if the handle was alive and is now destroyed.
    bool destroy(HandleType handle) {
        if (get(handle) == nullptr) return false;
        Slot& slot = slots_[handle.index];
        slot.alive = false;
        slot.generation++;
        slot.value = T{};
        freeList_.push_back(handle.index);
        return true;
    }

    [[nodiscard]] size_t aliveCount() const { return slots_.size() - freeList_.size(); }

    /// Visit every live element: f(HandleType, T&).
    template <typename F>
    void forEach(F&& f) {
        for (uint32_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].alive) f(HandleType{i, slots_[i].generation}, slots_[i].value);
    }

private:
    struct Slot {
        T value{};
        uint32_t generation = 1;
        bool alive = false;
    };
    std::vector<Slot> slots_;
    std::vector<uint32_t> freeList_;
};

} // namespace rendy
