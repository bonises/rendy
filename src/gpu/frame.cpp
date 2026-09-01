#include "gpu/frame.hpp"

namespace rendy::gpu {

FrameRing::FrameRing(Context& ctx, Swapchain& swapchain) : ctx_(ctx), swapchain_(swapchain) {
    for (PerFrame& frame : frames_) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = ctx_.graphicsFamily();
        VK_CHECK(vkCreateCommandPool(ctx_.device(), &poolInfo, nullptr, &frame.pool));

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = frame.pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx_.device(), &allocInfo, &frame.cmd));

        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(ctx_.device(), &semInfo, nullptr, &frame.acquireSemaphore));
    }

    renderFinished_.resize(swapchain_.imageCount());
    for (VkSemaphore& sem : renderFinished_) {
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(ctx_.device(), &semInfo, nullptr, &sem));
    }

    VkSemaphoreTypeCreateInfo typeInfo{};
    typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semInfo.pNext = &typeInfo;
    VK_CHECK(vkCreateSemaphore(ctx_.device(), &semInfo, nullptr, &timeline_));
}

FrameRing::~FrameRing() {
    ctx_.waitIdle();
    for (size_t i = 0; i < frames_.size(); ++i) flushDeletions(i);
    for (PerFrame& frame : frames_) {
        vkDestroySemaphore(ctx_.device(), frame.acquireSemaphore, nullptr);
        vkDestroyCommandPool(ctx_.device(), frame.pool, nullptr);
    }
    for (VkSemaphore sem : renderFinished_) vkDestroySemaphore(ctx_.device(), sem, nullptr);
    vkDestroySemaphore(ctx_.device(), timeline_, nullptr);
}

void FrameRing::flushDeletions(size_t slotIndex) {
    for (auto& fn : frames_[slotIndex].deletions) fn();
    frames_[slotIndex].deletions.clear();
}

void FrameRing::defer(std::function<void()> fn) {
    frames_[slot()].deletions.push_back(std::move(fn));
}

FrameRing::FrameInfo FrameRing::begin() {
    PerFrame& frame = frames_[slot()];

    // Wait until the GPU finished the work this slot submitted last time.
    if (frameCounter_ >= kFramesInFlight) {
        const uint64_t waitValue = frameCounter_ - kFramesInFlight + 1;
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &timeline_;
        waitInfo.pValues = &waitValue;
        VK_CHECK(vkWaitSemaphores(ctx_.device(), &waitInfo, UINT64_MAX));
    }
    flushDeletions(slot());

    // Acquire, recreating the swapchain when it's stale. If we still can't
    // acquire after a recreate, skip the frame instead of submitting with an
    // unsignaled semaphore (that would hang the queue).
    bool acquired = false;
    for (int attempt = 0; attempt < 2 && !acquired; ++attempt) {
        const VkResult result =
            vkAcquireNextImageKHR(ctx_.device(), swapchain_.handle(), UINT64_MAX,
                                  frame.acquireSemaphore, VK_NULL_HANDLE, &imageIndex_);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            acquired = true;
            break;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!swapchain_.recreate()) return {};
            if (renderFinished_.size() != swapchain_.imageCount()) {
                for (VkSemaphore sem : renderFinished_)
                    vkDestroySemaphore(ctx_.device(), sem, nullptr);
                renderFinished_.assign(swapchain_.imageCount(), VK_NULL_HANDLE);
                for (VkSemaphore& sem : renderFinished_) {
                    VkSemaphoreCreateInfo semInfo{};
                    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                    VK_CHECK(vkCreateSemaphore(ctx_.device(), &semInfo, nullptr, &sem));
                }
            }
            continue;
        }
        VK_CHECK(result);
    }
    if (!acquired) return {};

    vkResetCommandPool(ctx_.device(), frame.pool, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(frame.cmd, &beginInfo));

    frameActive_ = true;
    return {frame.cmd, imageIndex_, true};
}

void FrameRing::end() {
    // A skipped frame (minimized surface) submits nothing and must not
    // advance the ring: the timeline value it would wait on later was never
    // scheduled to be signaled.
    if (!frameActive_) return;
    PerFrame& frame = frames_[slot()];
    VK_CHECK(vkEndCommandBuffer(frame.cmd));

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = frame.acquireSemaphore;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalInfos[2]{};
    signalInfos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfos[0].semaphore = renderFinished_[imageIndex_];
    signalInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signalInfos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfos[1].semaphore = timeline_;
    signalInfos[1].value = frameCounter_ + 1;
    signalInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = frame.cmd;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &waitInfo;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 2;
    submit.pSignalSemaphoreInfos = signalInfos;
    VK_CHECK(vkQueueSubmit2(ctx_.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinished_[imageIndex_];
    VkSwapchainKHR swapchainHandle = swapchain_.handle();
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchainHandle;
    presentInfo.pImageIndices = &imageIndex_;

    const VkResult result = vkQueuePresentKHR(ctx_.graphicsQueue(), &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        // Handled by the next begin()'s acquire.
    } else {
        VK_CHECK(result);
    }

    frameActive_ = false;
    frameCounter_++;
}

} // namespace rendy::gpu
