#pragma once

#include "CommandPool.hpp"

namespace ZHLN::Vk {

template <Vk::QueueType QType>
inline CommandPool<QType>::CommandPool(const VkDevice device, const uint32_t queueFamily) {
    if (ZHLN_CreateCommandPool(device, queueFamily, &_raw)) {
        _device = device;
    }
}

template <Vk::QueueType QType>
inline CommandPool<QType>::~CommandPool() {
    if (_device != VK_NULL_HANDLE) {
        ZHLN_DestroyCommandPool(_device, &_raw);
    }
}

template <Vk::QueueType QType>
constexpr CommandPool<QType>::CommandPool(CommandPool&& other) noexcept:
    _device(std::exchange(other._device, VK_NULL_HANDLE)), _raw(std::exchange(other._raw, {})) {
}

template <Vk::QueueType QType>
inline auto CommandPool<QType>::operator=(CommandPool&& other) noexcept -> CommandPool<QType>& {
    if (this != &other) {
        if (_device != VK_NULL_HANDLE) {
            ZHLN_DestroyCommandPool(_device, &_raw);
        }
        _device = std::exchange(other._device, VK_NULL_HANDLE);
        _raw    = std::exchange(other._raw, {});
    }
    return *this;
}

template <Vk::QueueType QType>
inline auto CommandPool<QType>::Allocate(const uint32_t count) -> bool {
    if (!Valid()) {
        return false;
    }
    return ZHLN_AllocateCommandBuffers(_device, &_raw, count);
}

template <Vk::QueueType QType>
inline auto CommandPool<QType>::AllocateSecondary(const uint32_t count) -> bool {
    if (!Valid()) {
        return false;
    }
    return ZHLN_AllocateSecondaryCommandBuffers(_device, &_raw, count);
}

template <Vk::QueueType QType>
inline void CommandPool<QType>::Reset() noexcept {
    if (Valid()) {
        ZHLN_ResetCommandPool(_device, &_raw);
    }
}

template <uint32_t N, Vk::QueueType QType>
    requires(N > 0 && N <= 8)
inline auto CommandPools<N, QType>::Create(const VkDevice device, const Description& desc) noexcept -> CommandPools {
    CommandPools cp;
    for (auto& pool: cp._pools) {
        pool = CommandPool<QType>(device, desc.queue_family);
        if (!pool || !pool.Allocate(desc.buffers_per_pool)) {
            return {};
        }
    }
    return cp;
}
} // namespace ZHLN::Vk
