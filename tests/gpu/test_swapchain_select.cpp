// CPU-only tests for the pure swapchain selection logic (no device) —
// part of the regular rendy_tests suite, unlike test_gpu.cpp next door.

#include <catch2/catch_test_macros.hpp>

#include "gpu/swapchain_select.hpp"

using namespace rendy::gpu;

TEST_CASE("surface format: prefers BGRA8, accepts RGBA8, rejects exotics",
          "[gpu][swapchain]") {
    const VkSurfaceFormatKHR bgra{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    const VkSurfaceFormatKHR rgba{VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    const VkSurfaceFormatKHR unorm{VK_FORMAT_B8G8R8A8_UNORM,
                                   VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    const VkSurfaceFormatKHR hdr{VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                 VK_COLOR_SPACE_HDR10_ST2084_EXT};

    // BGRA preferred even when listed later.
    auto chosen = chooseSurfaceFormat({rgba, bgra});
    REQUIRE(chosen.has_value());
    REQUIRE(chosen->format == VK_FORMAT_B8G8R8A8_SRGB);

    // RGBA accepted when BGRA is absent.
    chosen = chooseSurfaceFormat({unorm, rgba});
    REQUIRE(chosen.has_value());
    REQUIRE(chosen->format == VK_FORMAT_R8G8B8A8_SRGB);

    // UNORM/HDR-only surfaces are rejected — the caller degrades explicitly
    // instead of silently mis-encoding colors or mis-reading screenshots.
    REQUIRE_FALSE(chooseSurfaceFormat({unorm, hdr}).has_value());
    REQUIRE_FALSE(chooseSurfaceFormat({}).has_value());

    // An sRGB format in a non-sRGB color space doesn't count.
    const VkSurfaceFormatKHR wrongSpace{VK_FORMAT_B8G8R8A8_SRGB,
                                        VK_COLOR_SPACE_HDR10_ST2084_EXT};
    REQUIRE_FALSE(chooseSurfaceFormat({wrongSpace}).has_value());
}

TEST_CASE("swapchain usage: transfer bits only when the surface has them",
          "[gpu][swapchain]") {
    // Full support → everything requested.
    VkImageUsageFlags usage = chooseSwapchainUsage(
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    REQUIRE((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0);
    REQUIRE((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0);
    REQUIRE((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0);
    REQUIRE((usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0); // never asked for

    // No TRANSFER_SRC on the surface → screenshots off, creation still valid.
    usage = chooseSwapchainUsage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    REQUIRE((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0);
    REQUIRE((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0);

    // Bare-minimum surface: only the guaranteed color attachment.
    usage = chooseSwapchainUsage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    REQUIRE(usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
}
