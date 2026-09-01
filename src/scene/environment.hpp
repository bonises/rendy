#pragma once

// Image-based lighting data baked from an equirectangular HDR: environment
// cubemap (background + source), cosine-convolved irradiance, GGX-prefiltered
// specular chain, and the split-sum BRDF LUT.

#include "gpu/context.hpp"
#include "rendy/core/result.hpp"

#include <memory>
#include <string>
#include <vector>

namespace rendy::detail {

inline constexpr uint32_t kEnvCubeSize = 512;
inline constexpr uint32_t kEnvCubeMips = 6;
inline constexpr uint32_t kIrradianceSize = 32;
inline constexpr uint32_t kPrefilterSize = 256;
inline constexpr uint32_t kPrefilterMips = 6;
inline constexpr uint32_t kBrdfLutSize = 512;

struct EnvironmentData {
    struct CubeImage {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE; // cube view for sampling
    };

    explicit EnvironmentData(gpu::Context& ctx) : ctx_(&ctx) {}
    ~EnvironmentData();
    EnvironmentData(const EnvironmentData&) = delete;
    EnvironmentData& operator=(const EnvironmentData&) = delete;

    CubeImage environment; // kEnvCubeSize, kEnvCubeMips
    CubeImage irradiance;  // kIrradianceSize, 1 mip
    CubeImage prefiltered; // kPrefilterSize, kPrefilterMips
    VkImage brdfLut = VK_NULL_HANDLE;
    VmaAllocation brdfLutAllocation = VK_NULL_HANDLE;
    VkImageView brdfLutView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE; // linear, clamp, full mip range

private:
    gpu::Context* ctx_;
};

/// Loads an .hdr file and bakes the full IBL set. Blocking (one-shot GPU
/// work); expect tens of milliseconds.
Result<std::shared_ptr<EnvironmentData>> bakeEnvironment(gpu::Context& ctx,
                                                         const std::string& hdrPath);

} // namespace rendy::detail
