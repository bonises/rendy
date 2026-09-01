#pragma once

// The 3D forward pass: HDR MSAA color + depth → resolve → tonemap onto the
// swapchain. One pipeline for opaque meshes; per-frame mapped buffers for
// transforms/materials/lights; CPU frustum culling.

#include "gpu/bindless.hpp"
#include "gpu/context.hpp"
#include "gpu/frame.hpp"
#include "scene/scene_impl.hpp"
#include "rendy/scene/camera.hpp"

namespace rendy::detail {

class Renderer3D {
public:
    Renderer3D(gpu::Context& ctx, gpu::BindlessTable& bindless, VkFormat swapchainFormat);
    ~Renderer3D();

    Renderer3D(const Renderer3D&) = delete;
    Renderer3D& operator=(const Renderer3D&) = delete;

    /// Records the whole 3D contribution for this frame: scene pass into HDR
    /// targets, resolve, tonemap onto `swapchainView`. Call before the 2D
    /// pass; the swapchain image must be in COLOR_ATTACHMENT_OPTIMAL.
    void render(VkCommandBuffer cmd, uint32_t slot, SceneImpl& scene, const Camera& camera,
                VkExtent2D extent, VkImageView swapchainView, gpu::FrameRing& frames);

    int tonemapper = 0;      ///< 0 neutral, 1 ACES, 2 off
    float exposure = 1.0f;

private:
    struct MappedBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
        size_t capacity = 0;
    };
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    void createPipelines(VkFormat swapchainFormat);
    void recreateTargets(VkExtent2D extent, gpu::FrameRing& frames);
    void destroyTargets();
    void ensureCapacity(MappedBuffer& buffer, size_t bytes, uint32_t slot,
                        gpu::FrameRing& frames);
    void updateDescriptors(uint32_t slot);

    gpu::Context& ctx_;
    gpu::BindlessTable& bindless_;

    static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    static constexpr VkSampleCountFlagBits kSamples = VK_SAMPLE_COUNT_4_BIT;

    VkExtent2D targetExtent_{0, 0};
    Target hdrMsaa_;
    Target hdrResolve_;
    Target depth_;
    VkSampler resolveSampler_ = VK_NULL_HANDLE;
    uint32_t hdrBindlessIndex_ = 0;

    VkDescriptorSetLayout frameSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet frameSets_[gpu::kFramesInFlight]{};
    bool descriptorsDirty_[gpu::kFramesInFlight]{};

    VkPipelineLayout meshLayout_ = VK_NULL_HANDLE;
    VkPipeline meshPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout tonemapLayout_ = VK_NULL_HANDLE;
    VkPipeline tonemapPipeline_ = VK_NULL_HANDLE;

    MappedBuffer uboBuffers_[gpu::kFramesInFlight];
    MappedBuffer transformBuffers_[gpu::kFramesInFlight];
    MappedBuffer materialBuffers_[gpu::kFramesInFlight];
    MappedBuffer lightBuffers_[gpu::kFramesInFlight];
};

} // namespace rendy::detail
