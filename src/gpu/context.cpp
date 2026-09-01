#include "gpu/context.hpp"

#include <cstring>
#include <string>

namespace rendy::gpu {
namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        log::error("[vulkan] {}", data->pMessage);
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        log::warn("[vulkan] {}", data->pMessage);
    else
        log::debug("[vulkan] {}", data->pMessage);
    return VK_FALSE;
}

bool hasLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& layer : layers)
        if (std::strcmp(layer.layerName, name) == 0) return true;
    return false;
}

// Score a physical device; <0 means unusable.
int scoreDevice(VkPhysicalDevice device, uint32_t* graphicsFamilyOut) {
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &features13;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features12;
    vkGetPhysicalDeviceFeatures2(device, &features2);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) return -1;

    const bool featuresOk =
        features13.dynamicRendering && features13.synchronization2 &&
        features12.timelineSemaphore && features12.descriptorIndexing &&
        features12.runtimeDescriptorArray && features12.descriptorBindingPartiallyBound &&
        features12.descriptorBindingSampledImageUpdateAfterBind &&
        features12.shaderSampledImageArrayNonUniformIndexing &&
        features2.features.samplerAnisotropy;
    if (!featuresOk) return -1;

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
    bool foundGraphics = false;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            *graphicsFamilyOut = i;
            foundGraphics = true;
            break;
        }
    }
    if (!foundGraphics) return -1;

    int score = 1;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
    else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 100;
    return score;
}

} // namespace

Result<std::unique_ptr<Context>> Context::create(const ContextConfig& config) {
    if (volkInitialize() != VK_SUCCESS)
        return err("Vulkan loader not found (is libvulkan.so.1 installed?)");

    auto ctx = std::unique_ptr<Context>(new Context());

    // ------------------------------------------------------------- instance
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "rendy-app";
    appInfo.pEngineName = "rendy";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = config.instanceExtensions;
    std::vector<const char*> layers;
    const bool validation = config.validation && hasLayer("VK_LAYER_KHRONOS_validation");
    if (config.validation && !validation)
        log::warn("validation requested but VK_LAYER_KHRONOS_validation is not installed");
    if (validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    instanceInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.data();

    const VkResult instanceResult = vkCreateInstance(&instanceInfo, nullptr, &ctx->instance_);
    if (instanceResult != VK_SUCCESS)
        return err("vkCreateInstance failed: VkResult {}", static_cast<int>(instanceResult));
    volkLoadInstance(ctx->instance_);

    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT debugInfo{};
        debugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugInfo.pfnUserCallback = debugCallback;
        VK_CHECK(vkCreateDebugUtilsMessengerEXT(ctx->instance_, &debugInfo, nullptr,
                                                &ctx->debugMessenger_));
        log::info("Vulkan validation layers enabled");
    }

    // ------------------------------------------------------ physical device
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(ctx->instance_, &deviceCount, nullptr);
    if (deviceCount == 0) return err("no Vulkan devices found");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(ctx->instance_, &deviceCount, devices.data());

    int bestScore = -1;
    for (VkPhysicalDevice candidate : devices) {
        uint32_t family = 0;
        const int score = scoreDevice(candidate, &family);
        if (score > bestScore) {
            bestScore = score;
            ctx->physicalDevice_ = candidate;
            ctx->graphicsFamily_ = family;
        }
    }
    if (bestScore < 0)
        return err("no Vulkan device supports the required 1.3 features "
                   "(dynamic rendering, sync2, timeline semaphores, descriptor indexing)");

    vkGetPhysicalDeviceProperties(ctx->physicalDevice_, &ctx->properties_);
    log::info("GPU: {} (Vulkan {}.{}.{})", ctx->properties_.deviceName,
              VK_API_VERSION_MAJOR(ctx->properties_.apiVersion),
              VK_API_VERSION_MINOR(ctx->properties_.apiVersion),
              VK_API_VERSION_PATCH(ctx->properties_.apiVersion));

    // --------------------------------------------------------------- device
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = ctx->graphicsFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &features13;
    features12.timelineSemaphore = VK_TRUE;
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features12;
    features2.features.samplerAnisotropy = VK_TRUE;
    features2.features.independentBlend = VK_TRUE;
    features2.features.imageCubeArray = VK_TRUE; // point light shadow cubes

    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &features2;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;

    const VkResult deviceResult =
        vkCreateDevice(ctx->physicalDevice_, &deviceInfo, nullptr, &ctx->device_);
    if (deviceResult != VK_SUCCESS)
        return err("vkCreateDevice failed: VkResult {}", static_cast<int>(deviceResult));
    volkLoadDevice(ctx->device_);
    vkGetDeviceQueue(ctx->device_, ctx->graphicsFamily_, 0, &ctx->graphicsQueue_);

    // ------------------------------------------------------------ allocator
    VmaVulkanFunctions vmaFunctions{};
    vmaFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vmaFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = ctx->physicalDevice_;
    allocatorInfo.device = ctx->device_;
    allocatorInfo.instance = ctx->instance_;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    allocatorInfo.pVulkanFunctions = &vmaFunctions;
    VK_CHECK(vmaCreateAllocator(&allocatorInfo, &ctx->allocator_));

    return ctx;
}

Context::~Context() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
    if (allocator_ != VK_NULL_HANDLE) vmaDestroyAllocator(allocator_);
    if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
    if (debugMessenger_ != VK_NULL_HANDLE)
        vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

} // namespace rendy::gpu
