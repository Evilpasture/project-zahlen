// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <string_view>
#include <vector>

#include "Device.hpp"
#include "Features.hpp"
#include "Instance.hpp"
#include "PhysicalDevice.hpp"

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
        return _device.Handle();
    }
    [[nodiscard]] auto GraphicsQueue() const noexcept -> VkQueue {
        return _device.GraphicsQueue();
    }
    [[nodiscard]] auto PresentQueue() const noexcept -> VkQueue {
        return _device.PresentQueue();
    }
    [[nodiscard]] auto TransferQueue() const noexcept -> VkQueue {
        return _device.TransferQueue();
    }
    [[nodiscard]] auto ComputeQueue() const noexcept -> VkQueue {
        return _device.ComputeQueue();
    }
    [[nodiscard]] auto Physical() const noexcept -> VkPhysicalDevice {
        return _physical.handle;
    }
    [[nodiscard]] auto PhysicalInfo() const noexcept -> const PhysicalDeviceInfo& {
        return _physical;
    }

    [[nodiscard]] auto BufferAddress(const VkBuffer buffer) const noexcept -> VkDeviceAddress {
        const VkBufferDeviceAddressInfo info {
            .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = buffer,
        };
        return vkGetBufferDeviceAddress(_device.Handle(), &info);
    }

    [[nodiscard]] auto DescriptorHeapsSupported() const noexcept -> bool {
        return _device.DescriptorHeapEnabled();
    }
    [[nodiscard]] auto MeshShadersSupported() const noexcept -> bool {
        return _device.MeshShaderEnabled();
    }
    [[nodiscard]] auto MeshShaderLimits() const noexcept -> Vk::MeshShaderLimits {
        return QueryMeshShaderLimits(_physical.handle);
    }
    [[nodiscard]] auto RayTracingSupported() const noexcept -> bool {
        return _device.RayTracingEnabled();
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
        return _device.Valid();
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

  private:
    Vk::Instance         _instanceObject {};
    VkSurfaceKHR         _surface = VK_NULL_HANDLE;
    PhysicalDeviceInfo   _physical {};
    LogicalDevice        _device {};
    DevicePresentSupport _present {};
    EnabledFeatureSet    _enabledFeatures;
    bool                 _addressBindingReportEnabled = false;
};

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
    constexpr Builder& ValidationMode(Vk::ValidationMode mode) noexcept {
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
    constexpr Builder& PhysicalDevice(const PhysicalDeviceInfo& physical) noexcept {
        _physical = physical;
        return *this;
    }
    constexpr Builder& InstanceExtensions(std::span<const std::string_view> extensions) noexcept {
        _instanceExtensions.assign(extensions.begin(), extensions.end());
        return *this;
    }
    constexpr Builder& DeviceExtensions(std::span<const char* const> extensions) noexcept {
        _deviceExtensions.assign(extensions.begin(), extensions.end());
        return *this;
    }
    constexpr Builder& DeviceExtensions(const std::vector<const char*>& extensions) noexcept {
        _deviceExtensions.assign(extensions.begin(), extensions.end());
        return *this;
    }

    template <typename... Ts>
    Builder& DeviceFeatures(FeatureChain<Ts...>& chain) noexcept {
        _features        = chain.GetRoot();
        _enabledFeatures = chain.SnapshotEnabled();
        return *this;
    }

    constexpr Builder& ScoreFunction(DeviceScoreFunction score, const void* userdata = nullptr) noexcept {
        _scoreFn       = score;
        _scoreUserdata = userdata;
        return *this;
    }

    [[nodiscard]] auto BuildInstance() noexcept -> std::expected<Vk::Instance, ErrorCode>;
    [[nodiscard]] auto SelectPhysicalDevice() const noexcept -> std::expected<PhysicalDeviceInfo, ErrorCode>;
    [[nodiscard]] auto Build() noexcept -> std::expected<Context, ErrorCode>;

  private:
    std::string_view      _appName = "ZHLN Engine";
    uint32_t              _appVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    Vk::ValidationMode    _validationMode = Vk::ValidationMode::On;
    Vk::Instance          _instanceObject {};
    VkInstance            _instanceView = VK_NULL_HANDLE;
    VkSurfaceKHR          _surface = VK_NULL_HANDLE;
    PhysicalDeviceInfo    _physical {};
    std::vector<std::string_view> _instanceExtensions;
    std::vector<const char*>      _deviceExtensions;
    const VkPhysicalDeviceFeatures2* _features = nullptr;
    EnabledFeatureSet _enabledFeatures;
    DeviceScoreFunction _scoreFn = nullptr;
    const void*         _scoreUserdata = nullptr;
};

} // namespace ZHLN::Vk
