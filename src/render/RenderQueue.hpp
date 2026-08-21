// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/render/RenderCode.hpp>

namespace ZHLN::Vk {

class Context; // Forward declaration

[[nodiscard]] std::expected<VkResult, VulkanCallError> WaitIdle(VkQueue queue) noexcept;

// NOLINTNEXTLINE(performance-enum-size)
enum class BarrierStage : VkPipelineStageFlags2 {
    StageNone = 0,
    Compute   = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    Fragment  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
    Vertex    = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
    Indirect  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
    Transfer  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
    Host      = VK_PIPELINE_STAGE_2_HOST_BIT
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
    DepthWrite    = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
};

// Enable bitwise OR operations on the scoped enums
[[nodiscard]] constexpr auto operator|(BarrierStage a, BarrierStage b) noexcept -> BarrierStage;
[[nodiscard]] constexpr auto operator|(BarrierAccess a, BarrierAccess b) noexcept -> BarrierAccess;

/**
 * @brief Unified memory barrier dispatcher.
 * Exposed early to resolve cyclic header dependencies between Queue and Core headers.
 */
inline void MemoryBarrier(VkCommandBuffer cmd, const ZHLN_MemoryBarrierDesc& desc) noexcept;

struct BarrierBuilder {
    VkPipelineStageFlags2 srcStage  = 0;
    VkAccessFlags2        srcAccess = 0;

    constexpr BarrierBuilder() noexcept;
    constexpr auto From(BarrierStage stage, BarrierAccess access) noexcept -> BarrierBuilder&;
    void           To(VkCommandBuffer cmd, BarrierStage dstStage, BarrierAccess dstAccess) const noexcept;
};

enum class QueueType : uint8_t { Graphics, Compute, Transfer };

// Primary templates (default to invalid/false)
template <QueueType Queue, BarrierStage Stage>
struct IsStageValid: std::false_type {};

template <QueueType Queue, BarrierAccess Access>
struct IsAccessValid: std::false_type {};

// --- 1. GRAPHICS QUEUE: Supports all stages and accesses ---
template <BarrierStage Stage>
struct IsStageValid<QueueType::Graphics, Stage>: std::true_type {};

template <BarrierAccess Access>
struct IsAccessValid<QueueType::Graphics, Access>: std::true_type {};

// --- 2. COMPUTE QUEUE: Supports Compute, Transfer, and Host ---
template <>
struct IsStageValid<QueueType::Compute, BarrierStage::StageNone>: std::true_type {};
template <>
struct IsStageValid<QueueType::Compute, BarrierStage::Compute>: std::true_type {};
template <>
struct IsStageValid<QueueType::Compute, BarrierStage::Transfer>: std::true_type {};
template <>
struct IsStageValid<QueueType::Compute, BarrierStage::Host>: std::true_type {};

template <BarrierAccess Access>
struct IsAccessValid<QueueType::Compute, Access>:
    std::bool_constant<
        (static_cast<VkAccessFlags2>(Access) &
         ~(static_cast<VkAccessFlags2>(BarrierAccess::ShaderRead) | static_cast<VkAccessFlags2>(BarrierAccess::ShaderWrite) |
           static_cast<VkAccessFlags2>(BarrierAccess::TransferRead) | static_cast<VkAccessFlags2>(BarrierAccess::TransferWrite) |
           static_cast<VkAccessFlags2>(BarrierAccess::HostRead) | static_cast<VkAccessFlags2>(BarrierAccess::HostWrite))) == 0> {};

// --- 3. TRANSFER QUEUE: Only supports Transfer (Copy/Clear) ---
template <>
struct IsStageValid<QueueType::Transfer, BarrierStage::StageNone>: std::true_type {};
template <>
struct IsStageValid<QueueType::Transfer, BarrierStage::Transfer>: std::true_type {};

template <BarrierAccess Access>
struct IsAccessValid<QueueType::Transfer, Access>:
    std::bool_constant<
        (static_cast<VkAccessFlags2>(Access) &
         ~(static_cast<VkAccessFlags2>(BarrierAccess::TransferRead) | static_cast<VkAccessFlags2>(BarrierAccess::TransferWrite))) == 0> {};

template <QueueType Queue, BarrierStage Stage, BarrierAccess Access>
concept ValidQueueOperation = IsStageValid<Queue, Stage>::value && IsAccessValid<Queue, Access>::value;

template <QueueType QType>
struct CommandBuffer {
    VkCommandBuffer            handle     = VK_NULL_HANDLE;
    static constexpr QueueType queue_type = QType;

    // Implicit conversion to raw handle for driver API calls
                       operator VkCommandBuffer() const noexcept;
    [[nodiscard]] bool Valid() const noexcept;
};

template <QueueType QType, BarrierStage SrcStage, BarrierAccess SrcAccess>
    requires ValidQueueOperation<QType, SrcStage, SrcAccess>
struct ConstrainedBarrier {
    CommandBuffer<QType> cmd;

    template <BarrierStage DstStage, BarrierAccess DstAccess>
        requires ValidQueueOperation<QType, DstStage, DstAccess>
    void TransitionTo() const noexcept;
};

// Fluent helper function to start a barrier
template <BarrierStage SrcStage, BarrierAccess SrcAccess, QueueType QType>
[[nodiscard]] constexpr auto BeginBarrier(CommandBuffer<QType> cmd) noexcept;

struct BufferQueueBarrier {
    VkBufferMemoryBarrier2 release;
    VkBufferMemoryBarrier2 acquire;

    [[nodiscard]] static auto Create(const ZHLN_BufferQueueBarrierDesc& desc) noexcept -> BufferQueueBarrier;
};

inline void BufferBarrier(VkCommandBuffer cmd, const VkBufferMemoryBarrier2& barrier) noexcept;
inline void BufferBarrier(VkCommandBuffer cmd, std::span<const VkBufferMemoryBarrier2> barriers) noexcept;

inline void BufferBarrier(
    VkCommandBuffer cmd,
    VkBuffer        buffer,
    BarrierStage    srcStage,
    BarrierAccess   srcAccess,
    BarrierStage    dstStage,
    BarrierAccess   dstAccess
) noexcept {
    VkBufferMemoryBarrier2 barrier = {
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
    BufferBarrier(cmd, barrier);
}

template <QueueType QType>
[[nodiscard]] constexpr auto ResolveQueue(const Context& ctx) noexcept -> VkQueue;

/**
 * @brief Resolves the appropriate raw VkQueue from the context based on QueueType.
 */
template <QueueType QType>
[[nodiscard]] constexpr auto ResolveQueueFamily(const Context& ctx) noexcept -> uint32_t;

/**
 * @brief Submits a strongly-typed command buffer to its corresponding queue
 *        and blocks the CPU until execution completes.
 */
template <QueueType QType>
[[nodiscard]] std::expected<void, Error> SubmitAndWait(const Context& ctx, CommandBuffer<QType> cmd) noexcept;

} // namespace ZHLN::Vk

#include "RenderQueue.inl"
