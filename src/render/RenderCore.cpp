// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "RenderCore.hpp"
#include <cstdlib>
#include <print>

namespace ZHLN::Vk {

std::expected<VkResult, std::string> WaitIdle(VkDevice device) noexcept {
    auto res = vkDeviceWaitIdle(device);
    if (res != VK_SUCCESS) {
        return std::unexpected(ResultString(res));
    }
    return res;
}

std::string ReportVkError(VkResult result, const char* context, const std::source_location& location = std::source_location::current()) {
    return std::format(
        "[Vk Error] {}:{} in {}: {} failed with {}", location.file_name(), location.line(), location.function_name(), context, Vk::ResultString(result)
    );
}

[[noreturn]] void ReportSemaphoreBoundsError(uint32_t index, uint32_t count) noexcept {
    std::println(stderr, "[ZHLN::Vk] FATAL: SemaphorePool index {} out of bounds (Size: {})", index, count);
    std::abort();
}

void SubmitAndWait(VkQueue queue, VkCommandBuffer cmd, VkSemaphore waitSemaphore, uint64_t waitValue, VkPipelineStageFlags2 waitStage) noexcept {
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

    vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
}

} // namespace ZHLN::Vk
