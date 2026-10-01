// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN::Vk {


template <typename T, auto DeleterFn>
class DeviceHandle {
  public:
    DeviceHandle() noexcept = default;
    DeviceHandle(VkDevice device, T raw) noexcept;
    ~DeviceHandle() noexcept;

    DeviceHandle(const DeviceHandle&)                    = delete;
    auto operator=(const DeviceHandle&) -> DeviceHandle& = delete;

    DeviceHandle(DeviceHandle&& other) noexcept;
    auto operator=(DeviceHandle&& other) noexcept -> DeviceHandle&;

    [[nodiscard]] constexpr auto Get() const noexcept -> T;
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool;
    constexpr explicit           operator bool() const noexcept;
    [[nodiscard]] constexpr auto Release() noexcept -> T;

  private:
    VkDevice _device = VK_NULL_HANDLE;
    T        _raw    = VK_NULL_HANDLE;
};

using PipelineLayout = DeviceHandle<VkPipelineLayout, ZHLN_DestroyPipelineLayout>;
using Pipeline       = DeviceHandle<VkPipeline, ZHLN_DestroyPipeline>;
using PipelineCache  = DeviceHandle<VkPipelineCache, ZHLN_DestroyPipelineCache>;
using Semaphore      = DeviceHandle<VkSemaphore, ZHLN_DestroySemaphore>;
using Sampler        = DeviceHandle<VkSampler, ZHLN_DestroySampler>;

using AccelerationStructure = DeviceHandle<VkAccelerationStructureKHR, ZHLN_DestroyAS>;

}
#include "Handles.inl"
