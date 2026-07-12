// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "RenderCore.hpp"
#include "../detail/Reflection.hpp"
#include <Zahlen/render/RenderCode.hpp>
#include <cstdlib>
#include <print>
namespace ZHLN::Vk {

std::expected<VkResult, VulkanCallError> WaitIdle(VkDevice device) noexcept {
    auto res = vkDeviceWaitIdle(device);
    if (res != VK_SUCCESS) {
        return std::unexpected(VulkanCallError::VulkanCallFailed);
    }
    return res;
}

std::expected<VkResult, VulkanCallError> WaitIdle(VkQueue queue) noexcept {
    auto res = vkQueueWaitIdle(queue);
    if (res != VK_SUCCESS) {
        return std::unexpected(VulkanCallError::VulkanCallFailed);
    }
    return res;
}

std::expected<void, Error>
    QueueSubmit(VkQueue queue, VkCommandBuffer cmd, VkSemaphore waitSemaphore, uint64_t waitValue, VkPipelineStageFlags2 waitStage, VkFence fence) noexcept {
    VkCommandBufferSubmitInfo cmd_info = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext         = {},
        .commandBuffer = cmd,
        .deviceMask    = {},
    };

    VkSemaphoreSubmitInfo wait_info = {
        .sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext       = {},
        .semaphore   = waitSemaphore,
        .value       = waitValue,
        .stageMask   = waitStage,
        .deviceIndex = {},
    };

    uint32_t wait_count = (waitSemaphore != VK_NULL_HANDLE && waitValue > 0) ? 1 : 0;

    VkSubmitInfo2 submit = {
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext                    = {},
        .flags                    = {},
        .waitSemaphoreInfoCount   = wait_count,
        .pWaitSemaphoreInfos      = wait_count > 0 ? &wait_info : nullptr,
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &cmd_info,
        .signalSemaphoreInfoCount = {},
        .pSignalSemaphoreInfos    = {},
    };

    VkResult res = vkQueueSubmit2(queue, 1, &submit, fence);
    if (res != VK_SUCCESS) [[unlikely]] {
        return std::unexpected(res);
    }
    return {};
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
