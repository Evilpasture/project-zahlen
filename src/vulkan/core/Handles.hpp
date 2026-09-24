// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN::Vk {

// RAII Handles

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

// NOTE: RAII handles which call destructors from C. Inlinable with LTO.
// (DescriptorSetLayout / DescriptorPool handles were removed with the
// descriptor-set model; the frame consumes descriptor heaps instead.)
using ShaderModule   = DeviceHandle<VkShaderModule, ZHLN_DestroyShaderModule>;
using PipelineLayout = DeviceHandle<VkPipelineLayout, ZHLN_DestroyPipelineLayout>;
using Pipeline       = DeviceHandle<VkPipeline, ZHLN_DestroyPipeline>;
using PipelineCache  = DeviceHandle<VkPipelineCache, ZHLN_DestroyPipelineCache>;
using Semaphore      = DeviceHandle<VkSemaphore, ZHLN_DestroySemaphore>;
using Sampler        = DeviceHandle<VkSampler, ZHLN_DestroySampler>;

using ImageView = DeviceHandle<VkImageView, ZHLN_DestroyImageView>;

// Ray tracing: the BLAS/TLAS handle carries the device it was created on, so
// the owner (NativeMesh) retires it through the handle alone -- no device
// stamp, no manual destroy call.
using AccelerationStructure = DeviceHandle<VkAccelerationStructureKHR, ZHLN_DestroyAS>;

} // namespace ZHLN::Vk
#include "Handles.inl"
