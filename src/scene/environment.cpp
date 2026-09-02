#include "scene/environment.hpp"

#include "scene/env_baker.hpp"

#include "rendy/core/log.hpp"

#include "shaders/env_bake_vert_spv.h"
#include "shaders/env_brdf_lut_frag_spv.h"
#include "shaders/env_equirect_frag_spv.h"
#include "shaders/env_irradiance_frag_spv.h"
#include "shaders/env_prefilter_frag_spv.h"

#include <stb_image.h>

#include <cstring>
#include <vector>

namespace rendy::detail {
namespace {

constexpr VkFormat kEnvFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kLutFormat = VK_FORMAT_R16G16_SFLOAT;

void createCube(gpu::Context& ctx, EnvironmentData::CubeImage* cube, uint32_t size,
                uint32_t mips, VkImageUsageFlags extraUsage = 0) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = kEnvFormat;
    info.extent = {size, size, 1};
    info.mipLevels = mips;
    info.arrayLayers = 6;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extraUsage;
    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &info, &allocCreate, &cube->image,
                            &cube->allocation, nullptr));
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = cube->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = kEnvFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 6};
    VK_CHECK(vkCreateImageView(ctx.device(), &viewInfo, nullptr, &cube->view));
}

} // namespace

EnvironmentData::~EnvironmentData() {
    for (CubeImage* cube : {&environment, &irradiance, &prefiltered}) {
        if (cube->view) vkDestroyImageView(ctx_->device(), cube->view, nullptr);
        if (cube->image) vmaDestroyImage(ctx_->allocator(), cube->image, cube->allocation);
    }
    if (brdfLutView) vkDestroyImageView(ctx_->device(), brdfLutView, nullptr);
    if (brdfLut) vmaDestroyImage(ctx_->allocator(), brdfLut, brdfLutAllocation);
    if (sampler) vkDestroySampler(ctx_->device(), sampler, nullptr);
}

Result<std::shared_ptr<EnvironmentData>> bakeEnvironment(gpu::Context& ctx,
                                                         const std::string& hdrPath) {
    // ---- load the equirect HDR
    int width = 0;
    int height = 0;
    int channels = 0;
    float* pixels = stbi_loadf(hdrPath.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr)
        return err("environment: failed to load '{}': {}", hdrPath, stbi_failure_reason());

    auto env = std::make_shared<EnvironmentData>(ctx);
    EnvBaker baker(ctx, env_bake_vert_spv, env_bake_vert_spv_words);

    // Equirect source texture (RGBA32F).
    VkImage equirectImage = VK_NULL_HANDLE;
    VmaAllocation equirectAlloc = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocCreate{};
        allocCreate.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &info, &allocCreate, &equirectImage,
                                &equirectAlloc, nullptr));

        const size_t bytes = static_cast<size_t>(width) * height * 4 * sizeof(float);
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo stagingCreate{};
        stagingCreate.usage = VMA_MEMORY_USAGE_AUTO;
        stagingCreate.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                              VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo stagingInfo{};
        VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bufferInfo, &stagingCreate, &staging,
                                 &stagingAlloc, &stagingInfo));
        std::memcpy(stagingInfo.pMappedData, pixels, bytes);
    }
    stbi_image_free(pixels);

    createCube(ctx, &env->environment, kEnvCubeSize, kEnvCubeMips,
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    createCube(ctx, &env->irradiance, kIrradianceSize, 1);
    createCube(ctx, &env->prefiltered, kPrefilterSize, kPrefilterMips);
    {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = kLutFormat;
        info.extent = {kBrdfLutSize, kBrdfLutSize, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo allocCreate{};
        allocCreate.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &info, &allocCreate, &env->brdfLut,
                                &env->brdfLutAllocation, nullptr));
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = env->brdfLut;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kLutFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx.device(), &viewInfo, nullptr, &env->brdfLutView));
    }
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        VK_CHECK(vkCreateSampler(ctx.device(), &samplerInfo, nullptr, &env->sampler));
    }

    // ---- pipelines
    VkPipeline equirectPipeline = baker.makePipeline(
        env_equirect_frag_spv, env_equirect_frag_spv_words, kEnvFormat);
    VkPipeline irradiancePipeline = baker.makePipeline(
        env_irradiance_frag_spv, env_irradiance_frag_spv_words, kEnvFormat);
    VkPipeline prefilterPipeline = baker.makePipeline(
        env_prefilter_frag_spv, env_prefilter_frag_spv_words, kEnvFormat);
    VkPipeline lutPipeline =
        baker.makePipeline(env_brdf_lut_frag_spv, env_brdf_lut_frag_spv_words, kLutFormat);

    VkImageView equirectView = baker.faceView(equirectImage, 0, 0,
                                              VK_FORMAT_R32G32B32A32_SFLOAT);
    VkDescriptorSet equirectSet = baker.makeInputSet(equirectView, env->sampler);
    VkDescriptorSet envSet = baker.makeInputSet(env->environment.view, env->sampler);

    // ---- record
    baker.begin();

    // Upload equirect.
    envWholeImageBarrier(baker.cmd, equirectImage, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    vkCmdCopyBufferToImage(baker.cmd, staging, equirectImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    envWholeImageBarrier(baker.cmd, equirectImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Equirect → env cube mip 0.
    envWholeImageBarrier(baker.cmd, env->environment.image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    for (uint32_t face = 0; face < 6; ++face)
        baker.renderFace(equirectPipeline, equirectSet,
                         baker.faceView(env->environment.image, face, 0, kEnvFormat),
                         kEnvCubeSize, face, 0.0f);

    // Mip chain for the env cube (blit).
    envWholeImageBarrier(baker.cmd, env->environment.image,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    int32_t mipSize = static_cast<int32_t>(kEnvCubeSize);
    for (uint32_t mip = 1; mip < kEnvCubeMips; ++mip) {
        VkImageMemoryBarrier2 toSrc{};
        toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toSrc.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toSrc.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        toSrc.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.image = env->environment.image;
        toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1, 0, 6};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toSrc;
        vkCmdPipelineBarrier2(baker.cmd, &dep);

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 6};
        blit.srcOffsets[1] = {mipSize, mipSize, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 6};
        blit.dstOffsets[1] = {std::max(mipSize / 2, 1), std::max(mipSize / 2, 1), 1};
        vkCmdBlitImage(baker.cmd, env->environment.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       env->environment.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
        mipSize = std::max(mipSize / 2, 1);
    }
    // After the blit chain, mips [0, N-1) are TRANSFER_SRC and the last is
    // TRANSFER_DST — transition each range without discarding contents.
    {
        VkImageMemoryBarrier2 barriers[2]{};
        for (auto& barrier : barriers) {
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask =
                VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.image = env->environment.image;
        }
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kEnvCubeMips - 1, 0, 6};
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, kEnvCubeMips - 1, 1, 0, 6};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 2;
        dep.pImageMemoryBarriers = barriers;
        vkCmdPipelineBarrier2(baker.cmd, &dep);
    }

    // Irradiance.
    envWholeImageBarrier(baker.cmd, env->irradiance.image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    for (uint32_t face = 0; face < 6; ++face)
        baker.renderFace(irradiancePipeline, envSet,
                         baker.faceView(env->irradiance.image, face, 0, kEnvFormat),
                         kIrradianceSize, face, 0.0f);
    envWholeImageBarrier(baker.cmd, env->irradiance.image,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Prefiltered chain.
    envWholeImageBarrier(baker.cmd, env->prefiltered.image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    for (uint32_t mip = 0; mip < kPrefilterMips; ++mip) {
        const uint32_t size = kPrefilterSize >> mip;
        const float roughness = static_cast<float>(mip) / (kPrefilterMips - 1);
        for (uint32_t face = 0; face < 6; ++face)
            baker.renderFace(prefilterPipeline, envSet,
                             baker.faceView(env->prefiltered.image, face, mip, kEnvFormat),
                             size, face, roughness);
    }
    envWholeImageBarrier(baker.cmd, env->prefiltered.image,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // BRDF LUT.
    envWholeImageBarrier(baker.cmd, env->brdfLut, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    baker.renderFace(lutPipeline, VK_NULL_HANDLE, env->brdfLutView, kBrdfLutSize, 0, 0.0f);
    envWholeImageBarrier(baker.cmd, env->brdfLut, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    baker.submitAndWait();

    vmaDestroyBuffer(ctx.allocator(), staging, stagingAlloc);
    vmaDestroyImage(ctx.allocator(), equirectImage, equirectAlloc);

    log::info("environment: baked '{}' ({}x{} HDR → {}³ cube, {} prefilter mips)", hdrPath,
              width, height, kEnvCubeSize, kPrefilterMips);
    return env;
}

} // namespace rendy::detail
