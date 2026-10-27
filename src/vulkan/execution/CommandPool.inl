// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "CommandPool.hpp"
#include <utility>

namespace ZHLN::Vk {

template <Vk::QueueType QType>
inline auto CommandPool<QType>::Create(const VkDevice device, const uint32_t queueFamily) noexcept
    -> std::expected<CommandPool, ErrorCode> {
    if (device == VK_NULL_HANDLE) [[unlikely]] {
        return std::unexpected(CommandPoolError::NullDevice);
    }
    const VkCommandPoolCreateInfo info {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamily,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    if (const VkResult res = vkCreateCommandPool(device, &info, nullptr, &pool); res != VK_SUCCESS) [[unlikely]] {
        return std::unexpected(ToError(res));
    }

    CommandPool result;
    result._device = device;
    result._pool   = pool;
    return result;
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
        _device  = std::exchange(other._device, VK_NULL_HANDLE);
        _pool    = std::exchange(other._pool, VK_NULL_HANDLE);
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
inline auto CommandPool<QType>::AllocateLevel(const uint32_t count, const VkCommandBufferLevel level) noexcept
    -> std::expected<void, ErrorCode> {
    if (!Valid()) [[unlikely]] {
        return std::unexpected(CommandPoolError::PoolNotReady);
    }
    if (count == 0) {
        return {};
    }

    std::vector<VkCommandBuffer>      allocated(count, VK_NULL_HANDLE);
    const VkCommandBufferAllocateInfo info {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = _pool,
        .level              = level,
        .commandBufferCount = count,
    };
    if (const VkResult res = vkAllocateCommandBuffers(_device, &info, allocated.data()); res != VK_SUCCESS) [[unlikely]] {
        return std::unexpected(ToError(res));
    }
    _buffers.insert(_buffers.end(), allocated.begin(), allocated.end());
    return {};
}

template <Vk::QueueType QType>
inline auto CommandPool<QType>::Reset(const VkCommandPoolResetFlags flags) noexcept -> std::expected<void, ErrorCode> {
    if (!Valid()) [[unlikely]] {
        return std::unexpected(CommandPoolError::PoolNotReady);
    }
    if (const VkResult res = vkResetCommandPool(_device, _pool, flags); res != VK_SUCCESS) [[unlikely]] {
        return std::unexpected(ToError(res));
    }
    return {};
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
inline auto CommandPools<N, QType>::Create(const VkDevice device, const Description& desc) noexcept
    -> std::expected<CommandPools, ErrorCode> {
    CommandPools pools;
    for (auto& pool: pools._pools) {
        auto created = CommandPool<QType>::Create(device, desc.queueFamily);
        if (!created) [[unlikely]] {
            return std::unexpected(created.error());
        }
        pool = std::move(*created);
        if (auto allocated = pool.Allocate(desc.buffersPerPool); !allocated) [[unlikely]] {
            return std::unexpected(allocated.error());
        }
    }
    return pools;
}

} // namespace ZHLN::Vk
