#include "gpu/texture.hpp"

#include <stb_image.h>

#include <algorithm>

namespace rendy::gpu {

TexturePool::TexturePool(Context& ctx, BindlessTable& bindless, Uploader& uploader)
    : ctx_(ctx), bindless_(bindless), uploader_(uploader) {
    // Index 0: 1x1 white, so untextured quads and invalid refs sample white.
    const uint8_t white[4] = {255, 255, 255, 255};
    auto result = createFromPixels(white, {1, 1}, {});
    if (!result || result.value().index != 0)
        log::error("bindless white texture did not land at index 0");
}

TexturePool::~TexturePool() {
    for (auto& [index, entry] : entries_) destroyEntry(entry);
}

void TexturePool::destroyEntry(Entry& entry) {
    vkDestroySampler(ctx_.device(), entry.sampler, nullptr);
    vkDestroyImageView(ctx_.device(), entry.view, nullptr);
    vmaDestroyImage(ctx_.allocator(), entry.image, entry.allocation);
}

Result<TextureRef> TexturePool::create(const void* pixels, IVec2 size, VkFormat format,
                                       uint32_t bytesPerPixel, const TextureOptions& options) {
    if (size.x <= 0 || size.y <= 0) return err("texture size must be positive, got {}x{}", size.x, size.y);
    const auto width = static_cast<uint32_t>(size.x);
    const auto height = static_cast<uint32_t>(size.y);

    Entry entry;
    entry.size = size;
    entry.format = format;

    uint32_t mipLevels = 1;
    if (options.mipmaps) {
        uint32_t maxDim = std::max(width, height);
        while (maxDim > 1) {
            maxDim /= 2;
            mipLevels++;
        }
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (mipLevels > 1) imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(ctx_.allocator(), &imageInfo, &allocCreate, &entry.image,
                            &entry.allocation, nullptr));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = entry.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
    if (format == VK_FORMAT_R8_UNORM)
        viewInfo.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                               VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R};
    VK_CHECK(vkCreateImageView(ctx_.device(), &viewInfo, nullptr, &entry.view));

    const bool linearFilter = options.filter == TextureOptions::Filter::Linear;
    const VkSamplerAddressMode addressMode = options.wrap == TextureOptions::Wrap::Repeat
                                                 ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                                 : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    samplerInfo.minFilter = samplerInfo.magFilter;
    samplerInfo.mipmapMode =
        options.mipmaps ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = addressMode;
    samplerInfo.addressModeV = addressMode;
    samplerInfo.addressModeW = addressMode;
    if (options.mipmaps) {
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        samplerInfo.anisotropyEnable = VK_TRUE;
        samplerInfo.maxAnisotropy = 16.0f;
    }
    VK_CHECK(vkCreateSampler(ctx_.device(), &samplerInfo, nullptr, &entry.sampler));

    uploader_.uploadImage(entry.image, pixels,
                          static_cast<size_t>(width) * height * bytesPerPixel, width, height,
                          mipLevels);

    const uint32_t index = bindless_.add(entry.view, entry.sampler);
    entries_.emplace(index, entry);
    return TextureRef{index, size};
}

Result<TextureRef> TexturePool::createFromPixels(const void* rgba, IVec2 size,
                                                 const TextureOptions& options) {
    return create(rgba, size,
                  options.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, 4,
                  options);
}

Result<TextureRef> TexturePool::createR8(const void* pixels, IVec2 size) {
    TextureOptions options;
    options.srgb = false;
    return create(pixels, size, VK_FORMAT_R8_UNORM, 1, options);
}

Result<TextureRef> TexturePool::loadFromFile(const std::string& path,
                                             const TextureOptions& options) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr)
        return err("failed to load image '{}': {}", path, stbi_failure_reason());
    auto result = createFromPixels(pixels, {width, height}, options);
    stbi_image_free(pixels);
    return result;
}

void TexturePool::update(TextureRef ref, const void* pixels, size_t bytes) {
    auto it = entries_.find(ref.index);
    if (it == entries_.end()) return;
    uploader_.uploadImage(it->second.image, pixels, bytes,
                          static_cast<uint32_t>(it->second.size.x),
                          static_cast<uint32_t>(it->second.size.y));
}

void TexturePool::destroy(TextureRef ref) {
    auto it = entries_.find(ref.index);
    if (it == entries_.end()) return;
    destroyEntry(it->second);
    bindless_.remove(ref.index);
    entries_.erase(it);
}

} // namespace rendy::gpu
