// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Device.hpp"
#include <utility>

namespace ZHLN::Vk {

LogicalDevice::LogicalDevice(
    const VkDevice handle,
    const VkQueue graphics,
    const VkQueue present,
    const VkQueue transfer,
    const VkQueue compute,
    const bool descriptorHeapEnabled,
    const bool meshShaderEnabled,
    const bool rayTracingEnabled
) noexcept:
    _handle(handle),
    _graphicsQueue(graphics),
    _presentQueue(present),
    _transferQueue(transfer),
    _computeQueue(compute),
    _descriptorHeapEnabled(descriptorHeapEnabled),
    _meshShaderEnabled(meshShaderEnabled),
    _rayTracingEnabled(rayTracingEnabled) {}

LogicalDevice::~LogicalDevice() noexcept {
    Reset();
}

LogicalDevice::LogicalDevice(LogicalDevice&& other) noexcept:
    _handle(std::exchange(other._handle, VK_NULL_HANDLE)),
    _graphicsQueue(std::exchange(other._graphicsQueue, VK_NULL_HANDLE)),
    _presentQueue(std::exchange(other._presentQueue, VK_NULL_HANDLE)),
    _transferQueue(std::exchange(other._transferQueue, VK_NULL_HANDLE)),
    _computeQueue(std::exchange(other._computeQueue, VK_NULL_HANDLE)),
    _descriptorHeapEnabled(std::exchange(other._descriptorHeapEnabled, false)),
    _meshShaderEnabled(std::exchange(other._meshShaderEnabled, false)),
    _rayTracingEnabled(std::exchange(other._rayTracingEnabled, false)) {}

auto LogicalDevice::operator=(LogicalDevice&& other) noexcept -> LogicalDevice& {
    if (this != &other) {
        Reset();
        _handle                = std::exchange(other._handle, VK_NULL_HANDLE);
        _graphicsQueue         = std::exchange(other._graphicsQueue, VK_NULL_HANDLE);
        _presentQueue          = std::exchange(other._presentQueue, VK_NULL_HANDLE);
        _transferQueue         = std::exchange(other._transferQueue, VK_NULL_HANDLE);
        _computeQueue          = std::exchange(other._computeQueue, VK_NULL_HANDLE);
        _descriptorHeapEnabled = std::exchange(other._descriptorHeapEnabled, false);
        _meshShaderEnabled     = std::exchange(other._meshShaderEnabled, false);
        _rayTracingEnabled     = std::exchange(other._rayTracingEnabled, false);
    }
    return *this;
}

void LogicalDevice::Reset() noexcept {
    if (_handle != VK_NULL_HANDLE) {
        vkDestroyDevice(_handle, nullptr);
    }
    _handle                = VK_NULL_HANDLE;
    _graphicsQueue         = VK_NULL_HANDLE;
    _presentQueue          = VK_NULL_HANDLE;
    _transferQueue         = VK_NULL_HANDLE;
    _computeQueue          = VK_NULL_HANDLE;
    _descriptorHeapEnabled = false;
    _meshShaderEnabled     = false;
    _rayTracingEnabled     = false;
}

} // namespace ZHLN::Vk
