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
    VkPipelineStageFlags2 waitStage     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
) noexcept;

inline constexpr VkPipelineStageFlags2 kAsyncComputeConsumerStages =
    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

// Compact 24-byte representation of a semaphore synchronization point.
//
// Mirrors the ImageView/GeometryView design: only semantic state is stored and
// the 48-byte VkSemaphoreSubmitInfo (sType/pNext/deviceIndex + padding = 24 dead
// bytes) is synthesized on the stack at the vkQueueSubmit2 boundary.
struct SemaphoreSubmit {
    VkSemaphore           semaphore = VK_NULL_HANDLE;                       // 8 bytes
    uint64_t              value     = 0;                                    // 8 bytes (0 => binary semaphore)
    VkPipelineStageFlags2 stage     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; // 8 bytes

    // -------------------------------------------------------------------------
    // "Tell, Don't Ask" Factories
    // -------------------------------------------------------------------------
    [[nodiscard]] static constexpr auto
        Binary(VkSemaphore sem, VkPipelineStageFlags2 stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) noexcept -> SemaphoreSubmit {
        return {.semaphore = sem, .value = 0, .stage = stageMask};
    }

    [[nodiscard]] static constexpr auto
        Timeline(VkSemaphore sem, uint64_t val, VkPipelineStageFlags2 stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) noexcept -> SemaphoreSubmit {
        return {.semaphore = sem, .value = val, .stage = stageMask};
    }

    // Owning handles are only borrowed here: submitting never takes ownership.
    [[nodiscard]] static constexpr auto
        Binary(const Semaphore& sem, VkPipelineStageFlags2 stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) noexcept -> SemaphoreSubmit {
        return Binary(sem.Get(), stageMask);
    }

    [[nodiscard]] static constexpr auto
        Timeline(const Semaphore& sem, uint64_t val, VkPipelineStageFlags2 stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) noexcept -> SemaphoreSubmit {
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
            .stageMask   = stage,
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
    VkPipelineStageFlags2 waitStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    VkSemaphore signalSemaphore = VK_NULL_HANDLE,
    uint64_t signalValue = 0,
    VkPipelineStageFlags2 signalStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
) noexcept -> std::expected<void, ErrorCode>;

// NOLINTNEXTLINE(performance-enum-size)
enum class BarrierStage : VkPipelineStageFlags2 {
    StageNone                  = 0,
    TopOfPipe                  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
    BottomOfPipe               = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
    Compute                    = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    Fragment                   = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
    Vertex                     = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
    ColorAttachment            = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    EarlyFragment              = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
    LateFragment               = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
    Indirect                   = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
    Transfer                   = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
    Copy                       = VK_PIPELINE_STAGE_2_COPY_BIT,
    Clear                      = VK_PIPELINE_STAGE_2_CLEAR_BIT,
    Host                       = VK_PIPELINE_STAGE_2_HOST_BIT,
    AccelerationStructureBuild = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
};

// NOLINTNEXTLINE(performance-enum-size)
enum class BarrierAccess : VkAccessFlags2 {
    AccessNone    = 0,
    ShaderRead    = VK_ACCESS_2_SHADER_READ_BIT,
    ShaderWrite   = VK_ACCESS_2_SHADER_WRITE_BIT,
    IndirectRead  = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
    TransferRead  = VK_ACCESS_2_TRANSFER_READ_BIT,
    TransferWrite = VK_ACCESS_2_TRANSFER_WRITE_BIT,
    HostRead      = VK_ACCESS_2_HOST_READ_BIT,
    HostWrite     = VK_ACCESS_2_HOST_WRITE_BIT,
    ColorRead     = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
    ColorWrite    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
    DepthRead     = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
    DepthWrite    = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,

    AccelerationStructureRead  = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
    AccelerationStructureWrite = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR
};

[[nodiscard]] constexpr auto operator|(BarrierStage a, BarrierStage b) noexcept -> BarrierStage;
[[nodiscard]] constexpr auto operator|(BarrierAccess a, BarrierAccess b) noexcept -> BarrierAccess;

inline void PipelineBarrier(
    VkCommandBuffer cmd,
    std::span<const VkBufferMemoryBarrier2> buffers = {},
    std::span<const VkImageMemoryBarrier2>  images  = {},
    std::span<const VkMemoryBarrier2>       memory  = {}
) noexcept;

enum class QueueType : uint8_t { Graphics, Compute, Transfer };

template <QueueType QType>
struct CommandBuffer {
    VkCommandBuffer            handle     = VK_NULL_HANDLE;
    static constexpr QueueType queue_type = QType;

                       operator VkCommandBuffer() const noexcept;
    [[nodiscard]] bool Valid() const noexcept;
};

[[nodiscard]] constexpr auto MakeBufferBarrier(
    VkBuffer      buffer,
    BarrierStage  srcStage,
    BarrierAccess srcAccess,
    BarrierStage  dstStage,
    BarrierAccess dstAccess
) noexcept -> VkBufferMemoryBarrier2 {
    return {
        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .pNext               = nullptr,
        .srcStageMask        = static_cast<VkPipelineStageFlags2>(srcStage),
        .srcAccessMask       = static_cast<VkAccessFlags2>(srcAccess),
        .dstStageMask        = static_cast<VkPipelineStageFlags2>(dstStage),
        .dstAccessMask       = static_cast<VkAccessFlags2>(dstAccess),
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer              = buffer,
        .offset              = 0,
        .size                = VK_WHOLE_SIZE
    };
}

inline void BufferBarrier(
    VkCommandBuffer cmd,
    VkBuffer        buffer,
    BarrierStage    srcStage,
    BarrierAccess   srcAccess,
    BarrierStage    dstStage,
    BarrierAccess   dstAccess
) noexcept {
    const VkBufferMemoryBarrier2 barrier = MakeBufferBarrier(buffer, srcStage, srcAccess, dstStage, dstAccess);
    PipelineBarrier(cmd, std::span<const VkBufferMemoryBarrier2>(&barrier, 1));
}

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
    VkPipelineStageFlags2 waitStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    VkSemaphore signalSemaphore = VK_NULL_HANDLE,
    uint64_t signalValue = 0,
    VkPipelineStageFlags2 signalStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
) noexcept -> std::expected<void, ErrorCode> {
    return QueueSubmit(
        ResolveQueue<QType>(ctx), std::move(cmd), waitSemaphore, waitValue, waitStage, signalSemaphore, signalValue, signalStage
    );
}

}

#include "RenderQueue.inl"
