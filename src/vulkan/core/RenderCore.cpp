// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "RenderCore.hpp"
#include "RenderCore.h"
#include <cstdlib>
#include <print>
namespace ZHLN::Vk {

std::expected<void, Error> WaitIdle(VkDevice device) noexcept {
    auto res = vkDeviceWaitIdle(device);
    if (res == VK_ERROR_DEVICE_LOST) {
        return std::unexpected(VulkanCallError::DeviceLost);
    }
    if (res != VK_SUCCESS) {
        return std::unexpected(VulkanCallError::VulkanCallFailed);
    }
    return {};
}

std::expected<void, Error> WaitIdle(VkQueue queue) noexcept {
    auto res = vkQueueWaitIdle(queue);
    if (res == VK_ERROR_DEVICE_LOST) {
        return std::unexpected(VulkanCallError::DeviceLost);
    }
    if (res != VK_SUCCESS) {
        return std::unexpected(VulkanCallError::VulkanCallFailed);
    }
    return {};
}

std::expected<void, Error> QueueSubmit(
    VkQueue                                    queue,
    std::span<const VkCommandBufferSubmitInfo> cmds,
    std::span<const VkSemaphoreSubmitInfo>     waits,
    std::span<const VkSemaphoreSubmitInfo>     signals,
    VkFence                                    fence
) noexcept {
    const VkSubmitInfo2 submit = {
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount   = static_cast<uint32_t>(waits.size()),
        .pWaitSemaphoreInfos      = waits.empty() ? nullptr : waits.data(),
        .commandBufferInfoCount   = static_cast<uint32_t>(cmds.size()),
        .pCommandBufferInfos      = cmds.empty() ? nullptr : cmds.data(),
        .signalSemaphoreInfoCount = static_cast<uint32_t>(signals.size()),
        .pSignalSemaphoreInfos    = signals.empty() ? nullptr : signals.data(),
    };

    const VkResult res = vkQueueSubmit2(queue, 1, &submit, fence);
    if (res == VK_ERROR_DEVICE_LOST) [[unlikely]] {
        return std::unexpected(VulkanCallError::DeviceLost);
    }
    if (res != VK_SUCCESS) [[unlikely]] {
        return std::unexpected(VulkanCallError::VulkanCallFailed);
    }
    return {};
}

std::expected<void, Error> QueueSubmit(
    VkQueue               queue,
    VkCommandBuffer       cmd,
    VkSemaphore           waitSemaphore,
    uint64_t              waitValue,
    VkPipelineStageFlags2 waitStage,
    VkSemaphore           signalSemaphore,
    uint64_t              signalValue,
    VkPipelineStageFlags2 signalStage,
    VkFence               fence
) noexcept {
    const VkCommandBufferSubmitInfo cmd_info    = MakeCommandBufferSubmitInfo(cmd);
    const VkSemaphoreSubmitInfo     wait_info   = MakeSemaphoreSubmitInfo(waitSemaphore, waitValue, waitStage);
    const VkSemaphoreSubmitInfo     signal_info = MakeSemaphoreSubmitInfo(signalSemaphore, signalValue, signalStage);
    return QueueSubmit(
        queue,
        cmd != VK_NULL_HANDLE ? std::span<const VkCommandBufferSubmitInfo> {&cmd_info, 1} : std::span<const VkCommandBufferSubmitInfo> {},
        waitSemaphore != VK_NULL_HANDLE ? std::span<const VkSemaphoreSubmitInfo> {&wait_info, 1} : std::span<const VkSemaphoreSubmitInfo> {},
        signalSemaphore != VK_NULL_HANDLE ? std::span<const VkSemaphoreSubmitInfo> {&signal_info, 1} : std::span<const VkSemaphoreSubmitInfo> {},
        fence
    );
}

std::string ReportVkError(VkResult result, const char* context, const std::source_location& location) {
    return std::format(
        "[ZHLN::Vk] {}:{} in {}: {} failed with {}", location.file_name(), location.line(), location.function_name(), context,
        ZHLN::Reflect::EnumToString(result)
    );
}

[[noreturn]] void ReportSemaphoreBoundsError(uint32_t index, uint32_t count) noexcept {
    std::println(stderr, "[ZHLN::Vk] FATAL: SemaphorePool index {} out of bounds (Size: {})", index, count);
    std::abort();
}

std::expected<void, Error>
    SubmitAndWait(VkQueue queue, VkCommandBuffer cmd, VkSemaphore waitSemaphore, uint64_t waitValue, VkPipelineStageFlags2 waitStage) noexcept {
    auto submit_res = QueueSubmit(queue, cmd, waitSemaphore, waitValue, waitStage);
    if (!submit_res) [[unlikely]] {
        return submit_res;
    }

    auto wait_res = WaitIdle(queue);
    if (!wait_res) [[unlikely]] {
        return std::unexpected(wait_res.error());
    }
    return {};
}

} // namespace ZHLN::Vk
