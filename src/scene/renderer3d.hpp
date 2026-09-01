#pragma once

// The 3D forward pass: HDR MSAA color + depth → resolve → tonemap onto the
// swapchain. One pipeline for opaque meshes; per-frame mapped buffers for
// transforms/materials/lights; CPU frustum culling.

#include "gpu/bindless.hpp"
#include "gpu/context.hpp"
#include "gpu/frame.hpp"
#include "gpu/shader_blob.hpp"
#include "gpu/upload.hpp"
#include "scene/scene_impl.hpp"
#include "rendy/scene/camera.hpp"

#include <string_view>

namespace rendy::detail {

class Renderer3D {
public:
    Renderer3D(gpu::Context& ctx, gpu::BindlessTable& bindless, gpu::Uploader& uploader,
               VkFormat swapchainFormat);
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
    float shadowDistance = 60.0f; ///< directional shadows reach this far

    /// Hot reload: replace one of mesh/tonemap/shadow shaders by filename
    /// and rebuild the pipelines. Returns false for unknown names.
    bool reloadShader(std::string_view name, std::vector<uint32_t> spirv,
                      gpu::FrameRing& frames);

    static constexpr uint32_t kMaxCascades = 4;
    static constexpr uint32_t kMaxSpotShadows = 8;
    static constexpr uint32_t kMaxPointShadows = 4;

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

    void createLayouts();
    void createPipelines();
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
    VkPipeline meshBlendPipeline_ = VK_NULL_HANDLE; ///< alpha blend, no depth write
    VkPipelineLayout tonemapLayout_ = VK_NULL_HANDLE;
    VkPipeline tonemapPipeline_ = VK_NULL_HANDLE;
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipeline skyboxPipeline_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    gpu::ShaderBlob meshVertBlob_, meshFragBlob_, tonemapVertBlob_, tonemapFragBlob_,
        shadowVertBlob_, skyboxVertBlob_, skyboxFragBlob_;

    // Environment (IBL) binding state. Defaults are 1x1 black so the
    // descriptors are always valid.
    struct DefaultEnv {
        VkImage cube = VK_NULL_HANDLE;
        VmaAllocation cubeAllocation = VK_NULL_HANDLE;
        VkImageView cubeView = VK_NULL_HANDLE;
        VkImage lut = VK_NULL_HANDLE;
        VmaAllocation lutAllocation = VK_NULL_HANDLE;
        VkImageView lutView = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
    };
    DefaultEnv defaultEnv_;
    const EnvironmentData* boundEnvironment_ = nullptr;

    // Shadow map storage (fixed size, created up front).
    struct ShadowArray {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView sampleView = VK_NULL_HANDLE;          // array/cube-array view
        std::vector<VkImageView> layerViews;              // one 2D view per layer
        uint32_t size = 0;
        uint32_t layers = 0;
    };
    void createShadowArray(ShadowArray* array, uint32_t size, uint32_t layers, bool cube);
    void destroyShadowArray(ShadowArray* array);
    struct ShadowGroup {
        uint32_t baseTransform;
        uint32_t instances;
        MeshHandle mesh;
        uint32_t jointBase = 0xFFFFFFFFu; // kNoJoints
        uint32_t morphWeightBase = 0;
        uint32_t morphTargetCount = 0;
    };
    void renderShadowPass(VkCommandBuffer cmd, const ShadowArray& array, uint32_t layer,
                          const Mat4& lightViewProj, SceneImpl& scene,
                          const std::vector<ShadowGroup>& groups);

    ShadowArray cascadeShadows_; // 2048², kMaxCascades layers
    ShadowArray spotShadows_;    // 1024², kMaxSpotShadows layers
    ShadowArray pointShadows_;   // 512², kMaxPointShadows cubes
    VkSampler shadowSampler_ = VK_NULL_HANDLE;      // compare sampler
    VkSampler pointShadowSampler_ = VK_NULL_HANDLE; // plain sampler (manual compare)
    bool shadowsInSampleLayout_ = false;

    MappedBuffer uboBuffers_[gpu::kFramesInFlight];
    MappedBuffer transformBuffers_[gpu::kFramesInFlight];
    MappedBuffer materialBuffers_[gpu::kFramesInFlight];
    MappedBuffer lightBuffers_[gpu::kFramesInFlight];
    MappedBuffer jointBuffers_[gpu::kFramesInFlight];
    MappedBuffer morphWeightBuffers_[gpu::kFramesInFlight];
    VkBuffer boundMorphDeltaBuffer_ = VK_NULL_HANDLE; // scene's static delta SSBO
};

} // namespace rendy::detail
