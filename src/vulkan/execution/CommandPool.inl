// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "CommandPool.hpp"
#include <utility>

namespace ZHLN::Vk {

template <Vk::QueueType QType>
inline CommandPool<QType>::CommandPool(const VkDevice device, const uint32_t queueFamily) noexcept {
    if (device == VK_NULL_HANDLE) {
        return;
    }
    const VkCommandPoolCreateInfo info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamily,
    };
    if (vkCreateCommandPool(device, &info, nullptr, &_pool) == VK_SUCCESS) {
        _device = device;
    }
}

template <Vk::QueueType QType>
inline CommandPool<QType>::~CommandPool() noexcept {
    Destroy();
}

template <Vk::QueueType QType>
inline CommandPool<QType>::CommandPool(CommandPool&& other) noexcept:
    _device(std::exchange(other._device, VK_NULL_HANDLE)),
    _pool(std::exchange(other._pool, VK_NULL_HANDLE)),
    _buffers(std::move(other._buffers)) {
    other._buffers.clear();
}

template <Vk::QueueType QType>
inline auto CommandPool<QType>::operator=(CommandPool&& other) noexcept -> CommandPool& {
    if (this != &other) {
        Destroy();
        _device = std::exchange(other._device, VK_NULL_HANDLE);
        _pool   = std::exchange(other._pool, VK_NULL_HANDLE);
        _buffers = std::move(other._buffers);
        other._buffers.clear();
    }
    return *this;
}

template <Vk::QueueType QType>
inline auto CommandPool<QType>::EnsureValid() const noexcept -> std::expected<void, ErrorCode> {
    if (!Valid()) [[unlikely]] {
        return std::unexpected(CommandPoolError::PoolNotReady);
    }
    return {};
}

template <Vk::QueueType QType>
inline auto CommandPool<QType>::Allocate(const uint32_t count) noexcept -> std::expected<void, ErrorCode> {
    return AllocateLevel(count, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
}

template <Vk::QueueType QType>
inline auto CommandPool<QType>::AllocateSecondary(const uint32_t count) noexcept -> std::expected<void, ErrorCode> {
    return AllocateLevel(count, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
}

template <Vk::QueueType QType>
inline auto CommandPool<QType>::AllocateLevel(const uint32_t count, const VkCommandBufferLevel level) noexcept -> std::expected<void, ErrorCode> {
    if (auto valid = EnsureValid(); !valid) {
        return valid;
    }
    if (count == 0) {
        return {};
    }

    std::vector<VkCommandBuffer> allocated(count, VK_NULL_HANDLE);
    const VkCommandBufferAllocateInfo info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = _pool,
        .level = level,
        .commandBufferCount = count,
    };
    if (const VkResult result = vkAllocateCommandBuffers(_device, &info, allocated.data()); result != VK_SUCCESS) {
        return std::unexpected(CommandPoolError::CommandBufferAllocationFailed);
    }
    _buffers.insert(_buffers.end(), allocated.begin(), allocated.end());
    return {};
}

template <Vk::QueueType QType>
inline void CommandPool<QType>::Reset() noexcept {
    if (Valid()) {
        (void) vkResetCommandPool(_device, _pool, 0);
    }
}

template <Vk::QueueType QType>
inline void CommandPool<QType>::Destroy() noexcept {
    if (_device != VK_NULL_HANDLE && _pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(_device, _pool, nullptr);
    }
    _buffers.clear();
    _pool   = VK_NULL_HANDLE;
    _device = VK_NULL_HANDLE;
}

template <uint32_t N, Vk::QueueType QType>
    requires(N > 0)
inline auto CommandPools<N, QType>::Create(const VkDevice device, const Description& desc) noexcept -> CommandPools {
    CommandPools pools;
    for (auto& pool: pools._pools) {
        pool = CommandPool<QType>(device, desc.queueFamily);
        if (!pool || !pool.Allocate(desc.buffersPerPool)) {
            return {};
        }
    }
    return pools;
}

} // namespace ZHLN::Vk
