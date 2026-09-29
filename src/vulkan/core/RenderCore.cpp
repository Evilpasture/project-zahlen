// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "RenderCore.hpp"
#include "RenderCore.h"
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <cstdlib>
#include <print>
namespace ZHLN::Vk {

std::expected<void, ErrorCode> WaitIdle(VkDevice device) noexcept {
    const VkResult res = vkDeviceWaitIdle(device);
    if (res != VK_SUCCESS) {
        return std::unexpected(ToFrameError(res));
    }
    return {};
}

std::expected<void, ErrorCode> WaitIdle(VkQueue queue) noexcept {
    const VkResult res = vkQueueWaitIdle(queue);
    if (res != VK_SUCCESS) {
        return std::unexpected(ToFrameError(res));
    }
    return {};
}

namespace {

// Raw Vulkan submissions stay behind this C++ seam; public QueueSubmit
// consumes a token and cannot submit an in-progress command buffer.
std::expected<void, ErrorCode> SubmitInfos(
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
    if (res != VK_SUCCESS) [[unlikely]] {
        return std::unexpected(ToFrameError(res));
    }
    return {};
}

} // namespace

std::expected<void, ErrorCode> QueueSubmit(
    VkQueue                                queue,
    ExecutableCommands                    cmds,
    std::span<const VkSemaphoreSubmitInfo> waits,
    std::span<const VkSemaphoreSubmitInfo> signals,
    VkFence                                fence
) noexcept {
    if (!cmds) {
        return std::unexpected(CommandRecordingError::NotExecutable);
    }
    const VkCommandBufferSubmitInfo cmdInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmds.Handle(),
    };
    return SubmitInfos(queue, std::span<const VkCommandBufferSubmitInfo> {&cmdInfo, 1}, waits, signals, fence);
}

std::expected<void, ErrorCode> QueueSubmit(
    VkQueue               queue,
    ExecutableCommands    cmd,
    VkSemaphore           waitSemaphore,
    uint64_t              waitValue,
    VkPipelineStageFlags2 waitStage,
    VkSemaphore           signalSemaphore,
    uint64_t              signalValue,
    VkPipelineStageFlags2 signalStage,
    VkFence               fence
) noexcept {
    const VkSemaphoreSubmitInfo wait_info   = MakeSemaphoreSubmitInfo(waitSemaphore, waitValue, waitStage);
    const VkSemaphoreSubmitInfo signal_info = MakeSemaphoreSubmitInfo(signalSemaphore, signalValue, signalStage);
    return QueueSubmit(
        queue, std::move(cmd),
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

std::expected<void, ErrorCode>
    SubmitAndWait(VkQueue queue, ExecutableCommands cmd, VkSemaphore waitSemaphore, uint64_t waitValue, VkPipelineStageFlags2 waitStage) noexcept {
    auto submit_res = QueueSubmit(queue, std::move(cmd), waitSemaphore, waitValue, waitStage);
    if (!submit_res) [[unlikely]] {
        return submit_res;
    }

    auto wait_res = WaitIdle(queue);
    if (!wait_res) [[unlikely]] {
        return std::unexpected(wait_res.error());
    }
    return {};
}

}
