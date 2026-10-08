// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Rendering.hpp"
#include "RenderQueue.hpp"

namespace ZHLN::Vk {
namespace {

std::expected<void, Vk::Error> SubmitInfos(
    const VkQueue queue,
    const std::span<const VkCommandBufferSubmitInfo> commands,
    const std::span<const VkSemaphoreSubmitInfo> waits,
    const std::span<const VkSemaphoreSubmitInfo> signals,
    const VkFence fence
) noexcept {
    const VkSubmitInfo2 submit {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = static_cast<uint32_t>(waits.size()),
        .pWaitSemaphoreInfos = waits.empty() ? nullptr : waits.data(),
        .commandBufferInfoCount = static_cast<uint32_t>(commands.size()),
        .pCommandBufferInfos = commands.empty() ? nullptr : commands.data(),
        .signalSemaphoreInfoCount = static_cast<uint32_t>(signals.size()),
        .pSignalSemaphoreInfos = signals.empty() ? nullptr : signals.data(),
    };

    const VkResult result = vkQueueSubmit2(queue, 1, &submit, fence);
    if (result != VK_SUCCESS) [[unlikely]] {
        return std::unexpected(Vk::Error {result});
    }
    return {};
}

} // namespace

std::expected<void, Vk::Error> WaitIdle(const VkQueue queue) noexcept {
    const VkResult result = vkQueueWaitIdle(queue);
    if (result != VK_SUCCESS) {
        return std::unexpected(Vk::Error {result});
    }
    return {};
}

std::expected<void, Vk::Error> QueueSubmit(
    const VkQueue queue,
    ExecutableCommands commands,
    const std::span<const VkSemaphoreSubmitInfo> waits,
    const std::span<const VkSemaphoreSubmitInfo> signals,
    const VkFence fence
) noexcept {
    if (!commands) {
        return std::unexpected(CommandRecordingError::NotExecutable);
    }
    const VkCommandBufferSubmitInfo commandInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = commands.Handle(),
    };
    return SubmitInfos(queue, std::span<const VkCommandBufferSubmitInfo> {&commandInfo, 1}, waits, signals, fence);
}

std::expected<void, Vk::Error> QueueSubmit(
    const VkQueue queue,
    ExecutableCommands command,
    const VkSemaphore waitSemaphore,
    const uint64_t waitValue,
    const VkPipelineStageFlags2 waitStage,
    const VkSemaphore signalSemaphore,
    const uint64_t signalValue,
    const VkPipelineStageFlags2 signalStage,
    const VkFence fence
) noexcept {
    const VkSemaphoreSubmitInfo waitInfo = MakeSemaphoreSubmitInfo(waitSemaphore, waitValue, waitStage);
    const VkSemaphoreSubmitInfo signalInfo = MakeSemaphoreSubmitInfo(signalSemaphore, signalValue, signalStage);
    return QueueSubmit(
        queue,
        std::move(command),
        waitSemaphore != VK_NULL_HANDLE ? std::span<const VkSemaphoreSubmitInfo> {&waitInfo, 1}
                                       : std::span<const VkSemaphoreSubmitInfo> {},
        signalSemaphore != VK_NULL_HANDLE ? std::span<const VkSemaphoreSubmitInfo> {&signalInfo, 1}
                                          : std::span<const VkSemaphoreSubmitInfo> {},
        fence
    );
}

std::expected<void, Vk::Error> SubmitAndWait(
    const VkQueue queue,
    ExecutableCommands command,
    const VkSemaphore waitSemaphore,
    const uint64_t waitValue,
    const VkPipelineStageFlags2 waitStage
) noexcept {
    auto submitted = QueueSubmit(queue, std::move(command), waitSemaphore, waitValue, waitStage);
    if (!submitted) [[unlikely]] {
        return std::unexpected(submitted.error());
    }
    return WaitIdle(queue);
}

} // namespace ZHLN::Vk
