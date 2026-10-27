// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "RenderQueue.hpp"

namespace ZHLN::Vk {

constexpr auto operator|(BarrierStage a, BarrierStage b) noexcept -> BarrierStage {
    return static_cast<BarrierStage>(static_cast<std::underlying_type_t<BarrierStage>>(a) | static_cast<std::underlying_type_t<BarrierStage>>(b));
}

constexpr auto operator|(BarrierAccess a, BarrierAccess b) noexcept -> BarrierAccess {
    return static_cast<BarrierAccess>(static_cast<std::underlying_type_t<BarrierAccess>>(a) | static_cast<std::underlying_type_t<BarrierAccess>>(b));
}

template <QueueType QType>
CommandBuffer<QType>::operator VkCommandBuffer() const noexcept {
    return handle;
}

template <QueueType QType>
bool CommandBuffer<QType>::Valid() const noexcept {
    return handle != VK_NULL_HANDLE;
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
        return ctx.PhysicalInfo().graphicsFamily;
    } else if constexpr (QType == QueueType::Compute) {
        return ctx.PhysicalInfo().computeFamily;
    } else if constexpr (QType == QueueType::Transfer) {
        return ctx.PhysicalInfo().transferFamily;
    }
}

}
