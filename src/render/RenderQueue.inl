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

inline void MemoryBarrier(VkCommandBuffer cmd, const ZHLN_MemoryBarrierDesc& desc) noexcept {
    ZHLN_CmdMemoryBarrier(cmd, &desc);
}

inline void ComputeToComputeBarrier(VkCommandBuffer cmd) noexcept {
    MemoryBarrier(
        cmd, {.src_stage  = static_cast<VkPipelineStageFlags2>(BarrierStage::Compute),
              .src_access = static_cast<VkAccessFlags2>(BarrierAccess::ShaderWrite),
              .dst_stage  = static_cast<VkPipelineStageFlags2>(BarrierStage::Compute),
              .dst_access = static_cast<VkAccessFlags2>(BarrierAccess::ShaderRead)}
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
    const VkDependencyInfo dep_info = {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext                    = nullptr,
        .dependencyFlags          = 0,
        .memoryBarrierCount       = 0,
        .pMemoryBarriers          = nullptr,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers    = &barrier,
        .imageMemoryBarrierCount  = 0,
        .pImageMemoryBarriers     = nullptr,
    };
    vkCmdPipelineBarrier2(cmd, &dep_info);
}

inline void BufferBarrier(VkCommandBuffer cmd, std::span<const VkBufferMemoryBarrier2> barriers) noexcept {
    if (barriers.empty()) {
        return;
    }

    const VkDependencyInfo dep_info = {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext                    = nullptr,
        .dependencyFlags          = 0,
        .memoryBarrierCount       = 0,
        .pMemoryBarriers          = nullptr,
        .bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
        .pBufferMemoryBarriers    = barriers.data(),
        .imageMemoryBarrierCount  = 0,
        .pImageMemoryBarriers     = nullptr,
    };
    vkCmdPipelineBarrier2(cmd, &dep_info);
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
    VkQueue queue      = ResolveQueue<QType>(ctx);
    auto    submit_res = QueueSubmit(queue, cmd.handle);
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
