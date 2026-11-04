// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "RenderQueue.hpp"

namespace ZHLN::Vk {

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
