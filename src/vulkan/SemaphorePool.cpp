// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SemaphorePool.hpp"

namespace ZHLN::Vk {

// ============================================================================
// SemaphorePool Implementation
// ============================================================================

SemaphorePool::~SemaphorePool() noexcept {
    Cleanup();
}

SemaphorePool::SemaphorePool(SemaphorePool&& other) noexcept: _device(other._device), _count(other._count) {
    for (uint32_t i = 0; i < 6; ++i) {
        _semaphores[i]       = other._semaphores[i];
        other._semaphores[i] = VK_NULL_HANDLE;
    }
    other._device = VK_NULL_HANDLE;
    other._count  = 0;
}

auto SemaphorePool::operator=(SemaphorePool&& other) noexcept -> SemaphorePool& {
    if (this != &other) {
        Cleanup();
        _device = other._device;
        _count  = other._count;

        for (uint32_t i = 0; i < 6; ++i) {
            _semaphores[i]       = other._semaphores[i];
            other._semaphores[i] = VK_NULL_HANDLE;
        }

        other._device = VK_NULL_HANDLE;
        other._count  = 0;
    }
    return *this;
}

void SemaphorePool::Rebuild(const VkDevice device, const uint32_t count) noexcept {
    Cleanup();
    _device = device;
    _count  = ZHLN::Min(count, 6U);

    for (uint32_t i = 0; i < _count; ++i) {
        _semaphores[i] = ZHLN_CreateSemaphore(_device);
    }
}

auto SemaphorePool::operator[](const uint32_t index) const noexcept -> VkSemaphore {
    if (index >= _count) [[unlikely]] {
        ReportSemaphoreBoundsError(index, _count);
    }
    return _semaphores[index];
}

auto SemaphorePool::Count() const noexcept -> uint32_t {
    return _count;
}

auto SemaphorePool::Valid() const noexcept -> bool {
    return _device != VK_NULL_HANDLE;
}

void SemaphorePool::Cleanup() noexcept {
    if (_device == VK_NULL_HANDLE) {
        return;
    }

    auto* const d = _device;
    for (uint32_t i = 0; i < _count; ++i) {
        if (_semaphores[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(d, _semaphores[i], nullptr);
        }
    }

    _semaphores.fill(VK_NULL_HANDLE);
    _count  = 0;
    _device = VK_NULL_HANDLE;
}
} // namespace ZHLN::Vk
