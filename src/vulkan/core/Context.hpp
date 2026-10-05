// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>

#include "Features.hpp"
#include "Instance.hpp"

namespace ZHLN::Vk {

enum class ContextError : uint8_t {
    InstanceCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Vulkan instance creation failed">{}) = 1,
    NoSuitableDeviceFound ZHLN_ANNOTATION(ZHLN::Description<"No suitable Vulkan device found">{}),
};

struct DevicePresentSupport {
    bool fifoLatestReady = false;
    bool presentTiming = false;
    bool presentAtAbsoluteTime = false;
    bool presentId2 = false;
};

class Context {
  public:
    class Builder;

    Context() noexcept = default;
    ~Context() noexcept;

    Context(const Context&)                    = delete;
    auto operator=(const Context&) -> Context& = delete;

    Context(Context&& other) noexcept;
    auto operator=(Context&& other) noexcept -> Context&;

    [[nodiscard]] auto Instance() const noexcept -> VkInstance {
        return _instanceObject.Handle();
    }
    [[nodiscard]] auto Surface() const noexcept -> VkSurfaceKHR {
        return _surface;
    }
    [[nodiscard]] auto Device() const noexcept -> VkDevice {
        return _device.handle;
    }
    [[nodiscard]] auto GraphicsQueue() const noexcept -> VkQueue {
        return _device.graphics_queue;
    }
    [[nodiscard]] auto PresentQueue() const noexcept -> VkQueue {
        return _device.present_queue;
    }
    [[nodiscard]] auto TransferQueue() const noexcept -> VkQueue {
        return _device.transfer_queue;
    }
    [[nodiscard]] auto ComputeQueue() const noexcept -> VkQueue {
        return _device.compute_queue;
    }
    [[nodiscard]] auto Physical() const noexcept -> VkPhysicalDevice {
        return _physical.handle;
    }
    [[nodiscard]] auto PhysicalInfo() const noexcept -> const ZHLN_PhysicalDeviceInfo& {
        return _physical;
    }

    [[nodiscard]] auto BufferAddress(VkBuffer buffer) const noexcept -> VkDeviceAddress {
        return ZHLN_GetBufferDeviceAddress(_device.handle, buffer);
    }

    [[nodiscard]] auto DescriptorHeapsSupported() const noexcept -> bool {
        return _device.descriptor_heap_enabled;
    }

    [[nodiscard]] auto MeshShadersSupported() const noexcept -> bool {
        return _device.mesh_shader_enabled;
    }

    [[nodiscard]] auto MeshShaderLimits() const noexcept -> ZHLN_MeshShaderLimits {
        return ZHLN_QueryMeshShaderLimits(_physical.handle);
    }


    [[nodiscard]] auto RayTracingSupported() const noexcept -> bool {
        return _device.ray_tracing_enabled;
    }

    [[nodiscard]] auto DeviceAddressBindingReportEnabled() const noexcept -> bool {
        return _addressBindingReportEnabled;
    }

    [[nodiscard]] auto PresentSupport() const noexcept -> const DevicePresentSupport& {
        return _present;
    }

    template <typename FeatureStruct>
    [[nodiscard]] auto GetFeature() const noexcept -> const FeatureStruct* {
        return FindEnabledFeature<FeatureStruct>(_enabledFeatures);
    }

    template <typename FeatureStruct, typename Predicate>
    [[nodiscard]] auto HasFeature(Predicate&& predicate) const noexcept -> bool {
        const FeatureStruct* enabled = GetFeature<FeatureStruct>();
        return enabled != nullptr && predicate(*enabled);
    }

    [[nodiscard("Always verify context initialization; check Valid() before use")]]
    auto Valid() const noexcept -> bool {
        return _device.handle != VK_NULL_HANDLE;
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

  private:
    Vk::Instance            _instanceObject {};
    VkSurfaceKHR            _surface        = VK_NULL_HANDLE;
    ZHLN_PhysicalDeviceInfo _physical       = {};
    ZHLN_Device             _device         = {};
    DevicePresentSupport    _present        = {};
    EnabledFeatureSet       _enabledFeatures;
    bool                    _addressBindingReportEnabled = false;
};

using ValidationMode = ZHLN_ValidationMode;

class Context::Builder {
  public:
    constexpr Builder() noexcept = default;

    constexpr Builder& AppName(std::string_view name) noexcept {
        _appName = name;
        return *this;
    }

    constexpr Builder& AppVersion(uint32_t version) noexcept {
        _appVersion = version;
        return *this;
    }

    constexpr Builder& ValidationMode(ValidationMode mode) noexcept {
        _validationMode = mode;
        return *this;
    }

    constexpr Builder& Instance(VkInstance inst) noexcept {
        _instanceView = inst;
        return *this;
    }

    constexpr Builder& Instance(Vk::Instance&& inst) noexcept {
        _instanceObject = std::move(inst);
        _instanceView   = _instanceObject.Handle();
        return *this;
    }

    constexpr Builder& Surface(VkSurfaceKHR surf) noexcept {
        _surface = surf;
        return *this;
    }

    constexpr Builder& PhysicalDevice(const ZHLN_PhysicalDeviceInfo& physical) noexcept {
        _physical = physical;
        return *this;
    }

    constexpr Builder& InstanceExtensions(std::span<const std::string_view> exts) noexcept {
        _instanceExtensions.assign(exts.begin(), exts.end());
        return *this;
    }

    constexpr Builder& DeviceExtensions(std::span<const char* const> exts) noexcept {
        _deviceExtensions.assign(exts.begin(), exts.end());
        return *this;
    }

    constexpr Builder& DeviceExtensions(const std::vector<const char*>& exts) noexcept {
        _deviceExtensions.assign(exts.begin(), exts.end());
        return *this;
    }

    template <typename... Ts>
    Builder& DeviceFeatures(FeatureChain<Ts...>& chain) noexcept {
        _features        = chain.GetRoot();
        _enabledFeatures = chain.SnapshotEnabled();
        return *this;
    }

    constexpr Builder& ScoreFunction(ZHLN_DeviceScoreFn scoreFn, void* userdata = nullptr) noexcept {
        _scoreFn       = scoreFn;
        _scoreUserdata = userdata;
        return *this;
    }

    [[nodiscard]] std::expected<Vk::Instance, ZHLN::ErrorCode>            BuildInstance() noexcept;
    [[nodiscard]] std::expected<ZHLN_PhysicalDeviceInfo, ZHLN::ErrorCode> SelectPhysicalDevice() const noexcept;
    [[nodiscard]] std::expected<Context, ZHLN::ErrorCode>                 Build() noexcept;

  private:
    std::string_view   _appName        = "ZHLN Engine";
    uint32_t           _appVersion     = VK_MAKE_API_VERSION(0, 1, 0, 0);
    Vk::ValidationMode _validationMode = ZHLN_VALIDATION_ON;

    Vk::Instance            _instanceObject {};
    VkInstance              _instanceView  = VK_NULL_HANDLE;
    VkSurfaceKHR            _surface       = VK_NULL_HANDLE;
    ZHLN_PhysicalDeviceInfo _physical      = {};

    std::vector<std::string_view> _instanceExtensions;
    std::vector<const char*>      _deviceExtensions;
    const VkPhysicalDeviceFeatures2* _features = nullptr;
    EnabledFeatureSet  _enabledFeatures;
    ZHLN_DeviceScoreFn _scoreFn       = nullptr;
    void*              _scoreUserdata = nullptr;
};

}
