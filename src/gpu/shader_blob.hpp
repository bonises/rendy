#pragma once

// A SPIR-V blob that starts as a view of the embedded constexpr arrays and
// can be replaced by hot-reloaded bytecode at runtime.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rendy::gpu {

struct ShaderBlob {
    const uint32_t* data = nullptr;
    size_t words = 0;
    std::vector<uint32_t> owned;

    ShaderBlob() = default;
    ShaderBlob(const uint32_t* embedded, size_t wordCount)
        : data(embedded), words(wordCount) {}

    void replace(std::vector<uint32_t> spirv) {
        owned = std::move(spirv);
        data = owned.data();
        words = owned.size();
    }
};

} // namespace rendy::gpu
