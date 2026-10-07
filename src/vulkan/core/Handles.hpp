// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <utility>

namespace ZHLN::Vk {

// These stateless deleter declarations are needed by DeviceHandle's template;
// keep the Vulkan calls themselves out of the header in Handles.cpp.
struct DestroyPipelineLayout {
    void operator()(VkDevice device, VkPipelineLayout handle) const noexcept;
};

struct DestroyPipeline {
    void operator()(VkDevice device, VkPipeline handle) const noexcept;
};

struct DestroyPipelineCache {
    void operator()(VkDevice device, VkPipelineCache handle) const noexcept;
};

struct DestroySemaphore {
    void operator()(VkDevice device, VkSemaphore handle) const noexcept;
};

struct DestroySampler {
    void operator()(VkDevice device, VkSampler handle) const noexcept;
};

struct DestroyImageView {
    void operator()(VkDevice device, VkImageView handle) const noexcept;
};

struct DestroyAccelerationStructure {
    void operator()(VkDevice device, VkAccelerationStructureKHR handle) const noexcept;
};

template <typename T, typename Deleter>
class DeviceHandle {
  public:
    DeviceHandle() noexcept = default;
    DeviceHandle(VkDevice device, T raw) noexcept: _device(device), _raw(raw) {}
    ~DeviceHandle() noexcept {
        Reset();
    }

    DeviceHandle(const DeviceHandle&)                    = delete;
    auto operator=(const DeviceHandle&) -> DeviceHandle& = delete;

    DeviceHandle(DeviceHandle&& other) noexcept:
        _device(std::exchange(other._device, VK_NULL_HANDLE)), _raw(std::exchange(other._raw, VK_NULL_HANDLE)) {}

    auto operator=(DeviceHandle&& other) noexcept -> DeviceHandle& {
        if (this != &other) {
            Reset();
            _device = std::exchange(other._device, VK_NULL_HANDLE);
            _raw    = std::exchange(other._raw, VK_NULL_HANDLE);
        }
        return *this;
    }

    [[nodiscard]] constexpr auto Get() const noexcept -> T {
        return _raw;
    }
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _device != VK_NULL_HANDLE && _raw != VK_NULL_HANDLE;
    }
    constexpr explicit operator bool() const noexcept {
        return Valid();
    }

    // Relinquish ownership to the caller. Vulkan handles do not encode their
    // parent device, so clear it together with the raw handle.
    [[nodiscard]] auto Release() noexcept -> T {
        _device = VK_NULL_HANDLE;
        return std::exchange(_raw, VK_NULL_HANDLE);
    }

    void Reset() noexcept {
        if (_device != VK_NULL_HANDLE && _raw != VK_NULL_HANDLE) {
            Deleter {}(_device, _raw);
        }
        _device = VK_NULL_HANDLE;
        _raw    = VK_NULL_HANDLE;
    }

  private:
    VkDevice _device = VK_NULL_HANDLE;
    T        _raw    = VK_NULL_HANDLE;
};

using PipelineLayout        = DeviceHandle<VkPipelineLayout, DestroyPipelineLayout>;
using Pipeline              = DeviceHandle<VkPipeline, DestroyPipeline>;
using PipelineCache         = DeviceHandle<VkPipelineCache, DestroyPipelineCache>;
using Semaphore             = DeviceHandle<VkSemaphore, DestroySemaphore>;
using Sampler               = DeviceHandle<VkSampler, DestroySampler>;
using ImageViewHandle       = DeviceHandle<VkImageView, DestroyImageView>;
using AccelerationStructure = DeviceHandle<VkAccelerationStructureKHR, DestroyAccelerationStructure>;

} // namespace ZHLN::Vk
