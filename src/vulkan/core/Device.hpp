// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

class LogicalDevice {
  public:
    LogicalDevice() noexcept = default;
    LogicalDevice(
        VkDevice handle,
        VkQueue graphics,
        VkQueue present,
        VkQueue transfer,
        VkQueue compute,
        bool descriptorHeapEnabled,
        bool meshShaderEnabled,
        bool rayTracingEnabled
    ) noexcept;
    ~LogicalDevice() noexcept;

    LogicalDevice(const LogicalDevice&)                    = delete;
    auto operator=(const LogicalDevice&) -> LogicalDevice& = delete;

    LogicalDevice(LogicalDevice&& other) noexcept;
    auto operator=(LogicalDevice&& other) noexcept -> LogicalDevice&;

    [[nodiscard]] auto Handle() const noexcept -> VkDevice {
        return _handle;
    }
    [[nodiscard]] auto GraphicsQueue() const noexcept -> VkQueue {
        return _graphicsQueue;
    }
    [[nodiscard]] auto PresentQueue() const noexcept -> VkQueue {
        return _presentQueue;
    }
    [[nodiscard]] auto TransferQueue() const noexcept -> VkQueue {
        return _transferQueue;
    }
    [[nodiscard]] auto ComputeQueue() const noexcept -> VkQueue {
        return _computeQueue;
    }
    [[nodiscard]] auto DescriptorHeapEnabled() const noexcept -> bool {
        return _descriptorHeapEnabled;
    }
    [[nodiscard]] auto MeshShaderEnabled() const noexcept -> bool {
        return _meshShaderEnabled;
    }
    [[nodiscard]] auto RayTracingEnabled() const noexcept -> bool {
        return _rayTracingEnabled;
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _handle != VK_NULL_HANDLE;
    }

    void Reset() noexcept;

  private:
    VkDevice _handle = VK_NULL_HANDLE;
    VkQueue  _graphicsQueue = VK_NULL_HANDLE;
    VkQueue  _presentQueue = VK_NULL_HANDLE;
    VkQueue  _transferQueue = VK_NULL_HANDLE;
    VkQueue  _computeQueue = VK_NULL_HANDLE;
    bool     _descriptorHeapEnabled = false;
    bool     _meshShaderEnabled = false;
    bool     _rayTracingEnabled = false;
};

} // namespace ZHLN::Vk
