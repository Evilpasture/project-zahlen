// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../core/Handles.hpp"
#include "CommandRecorder.hpp"
#include <Zahlen/Error.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ZHLN::Vk {

class Context;

[[nodiscard]] std::expected<void, ErrorCode> WaitIdle(VkQueue queue) noexcept;

[[nodiscard]] std::expected<void, ErrorCode> SubmitAndWait(
    VkQueue               queue,
    ExecutableCommands    cmd,
    VkSemaphore           waitSemaphore = VK_NULL_HANDLE,
    uint64_t              waitValue     = 0,
    Vk::PipelineStage waitStage     = Vk::PipelineStage::AllCommands
) noexcept;

inline constexpr Vk::PipelineStage kAsyncComputeConsumerStages =
    Vk::PipelineStage::Indirect | Vk::PipelineStage::VertexInput | Vk::PipelineStage::Vertex |
    Vk::PipelineStage::Fragment | Vk::PipelineStage::Compute;

// Compact 24-byte representation of a semaphore synchronization point.
//
// Mirrors the ImageView/GeometryView design: only semantic state is stored and
// the 48-byte VkSemaphoreSubmitInfo (sType/pNext/deviceIndex + padding = 24 dead
// bytes) is synthesized on the stack at the vkQueueSubmit2 boundary.
struct SemaphoreSubmit {
    VkSemaphore           semaphore = VK_NULL_HANDLE;                       // 8 bytes
    uint64_t              value     = 0;                                    // 8 bytes (0 => binary semaphore)
    Vk::PipelineStage stage     = Vk::PipelineStage::AllCommands; // 8 bytes

    // -------------------------------------------------------------------------
    // "Tell, Don't Ask" Factories
    // -------------------------------------------------------------------------
    [[nodiscard]] static constexpr auto
        Binary(VkSemaphore sem, Vk::PipelineStage stageMask = Vk::PipelineStage::AllCommands) noexcept -> SemaphoreSubmit {
        return {.semaphore = sem, .value = 0, .stage = stageMask};
    }

    [[nodiscard]] static constexpr auto
        Timeline(VkSemaphore sem, uint64_t val, Vk::PipelineStage stageMask = Vk::PipelineStage::AllCommands) noexcept -> SemaphoreSubmit {
        return {.semaphore = sem, .value = val, .stage = stageMask};
    }

    // Owning handles are only borrowed here: submitting never takes ownership.
    [[nodiscard]] static constexpr auto
        Binary(const Semaphore& sem, Vk::PipelineStage stageMask = Vk::PipelineStage::AllCommands) noexcept -> SemaphoreSubmit {
        return Binary(sem.Get(), stageMask);
    }

    [[nodiscard]] static constexpr auto
        Timeline(const Semaphore& sem, uint64_t val, Vk::PipelineStage stageMask = Vk::PipelineStage::AllCommands) noexcept -> SemaphoreSubmit {
        return Timeline(sem.Get(), val, stageMask);
    }

    // -------------------------------------------------------------------------
    // Observers
    // -------------------------------------------------------------------------
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return semaphore != VK_NULL_HANDLE;
    }
    explicit constexpr operator bool() const noexcept {
        return Valid();
    }

    [[nodiscard]] constexpr auto IsTimeline() const noexcept -> bool {
        return value != 0;
    }

    // -------------------------------------------------------------------------
    // JIT Stack Synthesis
    // -------------------------------------------------------------------------
    /// Synthesizes the C struct on the stack; Vulkan only borrows it for the call.
    [[nodiscard]] constexpr auto AsSubmitInfo() const noexcept -> VkSemaphoreSubmitInfo {
        return {
            .sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext       = nullptr,
            .semaphore   = semaphore,
            .value       = value,
            .stageMask   = ToVk(stage),
            .deviceIndex = 0,
        };
    }

    friend constexpr auto operator==(const SemaphoreSubmit&, const SemaphoreSubmit&) noexcept -> bool = default;
};

static_assert(sizeof(SemaphoreSubmit) == 24, "SemaphoreSubmit must be exactly 24 bytes.");
static_assert(std::is_trivially_copyable_v<SemaphoreSubmit>);

[[nodiscard]] auto
    QueueSubmit(VkQueue queue, ExecutableCommands cmds, std::span<const SemaphoreSubmit> waits = {}, std::span<const SemaphoreSubmit> signals = {}) noexcept
    -> std::expected<void, ErrorCode>;

[[nodiscard]] auto QueueSubmit(
    VkQueue queue,
    ExecutableCommands cmd,
    VkSemaphore waitSemaphore = VK_NULL_HANDLE,
    uint64_t waitValue = 0,
    Vk::PipelineStage waitStage = Vk::PipelineStage::AllCommands,
    VkSemaphore signalSemaphore = VK_NULL_HANDLE,
    uint64_t signalValue = 0,
    Vk::PipelineStage signalStage = Vk::PipelineStage::AllCommands
) noexcept -> std::expected<void, ErrorCode>;

// BarrierStage/BarrierAccess are the RenderQueue-facing names for the
// seam-exported Vk::PipelineStage / Vk::Access (include/vulkan/Enums.hpp);
// the | / & / Has operators live there too.
using BarrierStage  = PipelineStage;
using BarrierAccess = Access;

enum class QueueType : uint8_t { Graphics, Compute, Transfer };

template <QueueType QType>
struct CommandBuffer {
    VkCommandBuffer            handle     = VK_NULL_HANDLE;
    static constexpr QueueType queue_type = QType;

                       operator VkCommandBuffer() const noexcept;
    [[nodiscard]] bool Valid() const noexcept;
};

template <QueueType QType>
[[nodiscard]] constexpr auto ResolveQueue(const Context& ctx) noexcept -> VkQueue;

template <QueueType QType>
[[nodiscard]] constexpr auto ResolveQueueFamily(const Context& ctx) noexcept -> uint32_t;

template <QueueType QType>
[[nodiscard]] inline auto QueueSubmit(
    const Context& ctx,
    ExecutableCommands cmd,
    VkSemaphore waitSemaphore = VK_NULL_HANDLE,
    uint64_t waitValue = 0,
    Vk::PipelineStage waitStage = Vk::PipelineStage::AllCommands,
    VkSemaphore signalSemaphore = VK_NULL_HANDLE,
    uint64_t signalValue = 0,
    Vk::PipelineStage signalStage = Vk::PipelineStage::AllCommands
) noexcept -> std::expected<void, ErrorCode> {
    return QueueSubmit(
        ResolveQueue<QType>(ctx), std::move(cmd), waitSemaphore, waitValue, waitStage, signalSemaphore, signalValue, signalStage
    );
}

}

#include "RenderQueue.inl"
