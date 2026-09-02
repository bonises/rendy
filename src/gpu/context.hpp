#pragma once

// GPU context: instance, physical/logical device, queues, VMA allocator.
// One Context per App, created before any other GPU object.

#include "gpu/vk_common.hpp"
#include "rendy/core/result.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace rendy::gpu {

struct ContextConfig {
    bool validation = false;
    // Instance extensions the windowing system needs (from SDL).
    std::vector<const char*> instanceExtensions;
};

class Context {
public:
    static Result<std::unique_ptr<Context>> create(const ContextConfig& config);
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }
    VmaAllocator allocator() const { return allocator_; }

    uint32_t graphicsFamily() const { return graphicsFamily_; }
    VkQueue graphicsQueue() const { return graphicsQueue_; }
    /// Persisted to disk ($XDG_CACHE_HOME/rendy) across runs.
    VkPipelineCache pipelineCache() const { return pipelineCache_; }

    const VkPhysicalDeviceProperties& properties() const { return properties_; }

    /// BC/S3TC compressed textures usable? (Optional in Vulkan; enabled at
    /// device creation when available.)
    bool supportsBcTextures() const { return bcTexturesSupported_; }

    void waitIdle() const { vkDeviceWaitIdle(device_); }

private:
    Context() = default;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    uint32_t graphicsFamily_ = 0;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    bool bcTexturesSupported_ = false;
};

} // namespace rendy::gpu
