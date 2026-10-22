// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderQueue.hpp"
#include "Rendering.hpp"
#include <memory>

namespace ZHLN::Vk {
namespace {

/// Maximum number of wait/signal semaphores the engine ever submits in one call.
/// Anything beyond this falls back to heap storage ([[unlikely]], never hit today).
inline constexpr size_t kInlineSemaphoreSubmitCapacity = 8;

/// Synthesizes VkSemaphoreSubmitInfo records on the stack for the duration of a
/// vkQueueSubmit2 call. Vulkan only borrows the array, so nothing here outlives
/// the submit; the common path performs no heap allocation at all.
class SemaphoreSubmitInfoScratch {
  public:
    explicit SemaphoreSubmitInfoScratch(const std::span<const SemaphoreSubmit> submits) noexcept {
        _count = submits.size();
        if (_count > kInlineSemaphoreSubmitCapacity) [[unlikely]] {
            _heapStorage = std::make_unique<VkSemaphoreSubmitInfo[]>(_count);
            _storage     = _heapStorage.get();
        } else {
            _storage = _inline.data();
        }
        for (size_t i = 0; i < _count; ++i) {
            _storage[i] = submits[i].AsSubmitInfo();
        }
    }

    [[nodiscard]] auto Infos() const noexcept -> std::span<const VkSemaphoreSubmitInfo> {
        return {_storage, _count};
    }

  private:
    // Deliberately not value-initialized: the constructor writes every element
    // below _count before Infos() exposes the array, so zeroing would be wasted work.
    std::array<VkSemaphoreSubmitInfo, kInlineSemaphoreSubmitCapacity> _inline;
    std::unique_ptr<VkSemaphoreSubmitInfo[]> _heapStorage;
    VkSemaphoreSubmitInfo*                   _storage = nullptr;
    size_t                                   _count   = 0;
};

std::expected<void, ErrorCode> SubmitInfos(
    const VkQueue queue,
    const std::span<const VkCommandBufferSubmitInfo> commands,
    const std::span<const VkSemaphoreSubmitInfo> waits,
    const std::span<const VkSemaphoreSubmitInfo> signals
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

    const VkResult result = vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) [[unlikely]] {
        return std::unexpected(ToError(result));
    }
    return {};
}

} // namespace

std::expected<void, ErrorCode> WaitIdle(const VkQueue queue) noexcept {
    const VkResult result = vkQueueWaitIdle(queue);
    if (result != VK_SUCCESS) {
        return std::unexpected(ToError(result));
    }
    return {};
}

std::expected<void, ErrorCode> QueueSubmit(
    const VkQueue                          queue,
    ExecutableCommands                     commands,
    const std::span<const SemaphoreSubmit> waits,
    const std::span<const SemaphoreSubmit> signals
) noexcept {
    if (!commands) {
        return std::unexpected(CommandRecordingError::NotExecutable);
    }

    // JIT synthesis on the stack: the semantic values become C structs only here.
    const SemaphoreSubmitInfoScratch waitInfos(waits);
    const SemaphoreSubmitInfoScratch signalInfos(signals);

    const VkCommandBufferSubmitInfo commandInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = commands.Handle(),
    };
    return SubmitInfos(queue, std::span<const VkCommandBufferSubmitInfo> {&commandInfo, 1}, waitInfos.Infos(), signalInfos.Infos());
}

std::expected<void, ErrorCode> QueueSubmit(
    const VkQueue queue,
    ExecutableCommands command,
    const VkSemaphore waitSemaphore,
    const uint64_t waitValue,
    const VkPipelineStageFlags2 waitStage,
    const VkSemaphore signalSemaphore,
    const uint64_t signalValue,
    const VkPipelineStageFlags2 signalStage
) noexcept {
    const SemaphoreSubmit waitInfo   = SemaphoreSubmit::Timeline(waitSemaphore, waitValue, waitStage);
    const SemaphoreSubmit signalInfo = SemaphoreSubmit::Timeline(signalSemaphore, signalValue, signalStage);
    return QueueSubmit(
        queue, std::move(command), waitSemaphore != VK_NULL_HANDLE ? std::span<const SemaphoreSubmit> {&waitInfo, 1} : std::span<const SemaphoreSubmit> {},
        signalSemaphore != VK_NULL_HANDLE ? std::span<const SemaphoreSubmit> {&signalInfo, 1} : std::span<const SemaphoreSubmit> {}
    );
}

std::expected<void, ErrorCode> SubmitAndWait(
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
