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

using ShaderModule   = DeviceHandle<VkShaderModule, ZHLN_DestroyShaderModule>;
using PipelineLayout = DeviceHandle<VkPipelineLayout, ZHLN_DestroyPipelineLayout>;
using Pipeline       = DeviceHandle<VkPipeline, ZHLN_DestroyPipeline>;
using PipelineCache  = DeviceHandle<VkPipelineCache, ZHLN_DestroyPipelineCache>;
using Semaphore      = DeviceHandle<VkSemaphore, ZHLN_DestroySemaphore>;
using Sampler        = DeviceHandle<VkSampler, ZHLN_DestroySampler>;

// Descriptor heaps need the exact view create info, not only the VkImageView
// handle. Keep it with the owned view so moves cannot separate the two.
class ImageView {
  public:
    ImageView() noexcept = default;
    ImageView(VkDevice device, VkImageView view, const VkImageViewCreateInfo& info) noexcept:
        _handle(device, view), _info(info) {
    }

    ImageView(const ImageView&)                    = delete;
    auto operator=(const ImageView&) -> ImageView& = delete;

    ImageView(ImageView&& other) noexcept:
        _handle(std::move(other._handle)), _info(std::exchange(other._info, VkImageViewCreateInfo {})) {
    }
    auto operator=(ImageView&& other) noexcept -> ImageView& {
        if (this != &other) {
            _handle = std::move(other._handle);
            _info = std::exchange(other._info, VkImageViewCreateInfo {});
        }
        return *this;
    }

    [[nodiscard]] auto Get() const noexcept -> VkImageView { return _handle.Get(); }
    [[nodiscard]] auto Info() const noexcept -> const VkImageViewCreateInfo& { return _info; }
    [[nodiscard]] auto Valid() const noexcept -> bool { return _handle.Valid(); }
    explicit operator bool() const noexcept { return Valid(); }
    [[nodiscard]] auto Release() noexcept -> VkImageView {
        _info = {};
        return _handle.Release();
    }

  private:
    DeviceHandle<VkImageView, ZHLN_DestroyImageView> _handle;
    VkImageViewCreateInfo _info {};
};

using AccelerationStructure = DeviceHandle<VkAccelerationStructureKHR, ZHLN_DestroyAS>;

}
#include "Handles.inl"
