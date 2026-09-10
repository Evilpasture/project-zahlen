// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "RenderQueue.hpp"

namespace ZHLN::Vk {

// Enable bitwise OR operations on the scoped enums
constexpr auto operator|(BarrierStage a, BarrierStage b) noexcept -> BarrierStage {
    return static_cast<BarrierStage>(static_cast<std::underlying_type_t<BarrierStage>>(a) | static_cast<std::underlying_type_t<BarrierStage>>(b));
}

constexpr auto operator|(BarrierAccess a, BarrierAccess b) noexcept -> BarrierAccess {
    return static_cast<BarrierAccess>(static_cast<std::underlying_type_t<BarrierAccess>>(a) | static_cast<std::underlying_type_t<BarrierAccess>>(b));
}

inline void PipelineBarrier(
    VkCommandBuffer cmd,
    std::span<const VkBufferMemoryBarrier2> buffers,
    std::span<const VkImageMemoryBarrier2>  images,
    std::span<const VkMemoryBarrier2>       memory
) noexcept {
    if (buffers.empty() && images.empty() && memory.empty()) {
        return;
    }
    const VkDependencyInfo dep_info = {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount       = static_cast<uint32_t>(memory.size()),
        .pMemoryBarriers          = memory.data(),
        .bufferMemoryBarrierCount = static_cast<uint32_t>(buffers.size()),
        .pBufferMemoryBarriers    = buffers.data(),
        .imageMemoryBarrierCount  = static_cast<uint32_t>(images.size()),
        .pImageMemoryBarriers     = images.data(),
    };
    vkCmdPipelineBarrier2(cmd, &dep_info);
}

inline void MemoryBarrier(VkCommandBuffer cmd, const ZHLN_MemoryBarrierDesc& desc) noexcept {
    const VkMemoryBarrier2 barrier = MakeMemoryBarrier(desc);
    PipelineBarrier(cmd, {}, {}, std::span<const VkMemoryBarrier2>(&barrier, 1));
}

inline void MemoryBarrier(
    VkCommandBuffer cmd, BarrierStage srcStage, BarrierAccess srcAccess, BarrierStage dstStage, BarrierAccess dstAccess
) noexcept {
    MemoryBarrier(
        cmd, {.src_stage  = static_cast<VkPipelineStageFlags2>(srcStage),
              .src_access = static_cast<VkAccessFlags2>(srcAccess),
              .dst_stage  = static_cast<VkPipelineStageFlags2>(dstStage),
              .dst_access = static_cast<VkAccessFlags2>(dstAccess)}
    );
}

template <QueueType QType>
CommandBuffer<QType>::operator VkCommandBuffer() const noexcept {
    return handle;
}

template <QueueType QType>
bool CommandBuffer<QType>::Valid() const noexcept {
    return handle != VK_NULL_HANDLE;
}

template <QueueType QType, BarrierStage SrcStage, BarrierAccess SrcAccess>
    requires ValidQueueOperation<QType, SrcStage, SrcAccess>
template <BarrierStage DstStage, BarrierAccess DstAccess>
    requires ValidQueueOperation<QType, DstStage, DstAccess>
void ConstrainedBarrier<QType, SrcStage, SrcAccess>::TransitionTo() const noexcept {
    MemoryBarrier(
        cmd.handle, {.src_stage  = static_cast<VkPipelineStageFlags2>(SrcStage),
                     .src_access = static_cast<VkAccessFlags2>(SrcAccess),
                     .dst_stage  = static_cast<VkPipelineStageFlags2>(DstStage),
                     .dst_access = static_cast<VkAccessFlags2>(DstAccess)}
    );
}

template <BarrierStage SrcStage, BarrierAccess SrcAccess, QueueType QType>
constexpr auto BeginBarrier(CommandBuffer<QType> cmd) noexcept {
    return ConstrainedBarrier<QType, SrcStage, SrcAccess> {cmd};
}

inline auto BufferQueueBarrier::Create(const ZHLN_BufferQueueBarrierDesc& desc) noexcept -> BufferQueueBarrier {
    auto raw = ZHLN_CreateBufferQueueBarrier(&desc);
    return {.release = raw.release, .acquire = raw.acquire};
}

inline void BufferBarrier(VkCommandBuffer cmd, const VkBufferMemoryBarrier2& barrier) noexcept {
    PipelineBarrier(cmd, std::span<const VkBufferMemoryBarrier2>(&barrier, 1));
}

inline void BufferBarrier(VkCommandBuffer cmd, std::span<const VkBufferMemoryBarrier2> barriers) noexcept {
    PipelineBarrier(cmd, barriers);
}

template <QueueType QType>
constexpr auto ResolveQueue(const Context& ctx) noexcept -> VkQueue {
    if constexpr (QType == QueueType::Graphics) {
        return ctx.GraphicsQueue();
    } else if constexpr (QType == QueueType::Compute) {
        return ctx.ComputeQueue();
    } else if constexpr (QType == QueueType::Transfer) {
        return ctx.TransferQueue();
    }
}

template <QueueType QType>
constexpr auto ResolveQueueFamily(const Context& ctx) noexcept -> uint32_t {
    if constexpr (QType == QueueType::Graphics) {
        return ctx.PhysicalInfo().graphics_family;
    } else if constexpr (QType == QueueType::Compute) {
        return ctx.PhysicalInfo().compute_family;
    } else if constexpr (QType == QueueType::Transfer) {
        return ctx.PhysicalInfo().transfer_family;
    }
}

template <QueueType QType>
std::expected<void, Error> SubmitAndWait(const Context& ctx, CommandBuffer<QType> cmd) noexcept {
    return SubmitAndWait(ResolveQueue<QType>(ctx), cmd.handle);
}

} // namespace ZHLN::Vk
