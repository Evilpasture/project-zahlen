// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
#include "Device.hpp"
#include "Features.hpp"
#include "Instance.hpp"
#include "PhysicalDevice.hpp"
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ZHLN::Vk {

enum class ContextError : uint8_t {
    InvalidInstance ZHLN_ANNOTATION(ZHLN::Description<"No valid Vulkan instance was supplied"> {}) = 1,
    LoaderInitializationFailed ZHLN_ANNOTATION(ZHLN::Description<"Vulkan loader initialization failed"> {}),
    MultipleInstancesUnsupported ZHLN_ANNOTATION(ZHLN::Description<"Only one Vulkan instance can use the global dispatch table"> {}),
    NoSuitableDeviceFound ZHLN_ANNOTATION(ZHLN::Description<"No suitable Vulkan device found"> {}),
};

struct DevicePresentSupport {
    bool fifoLatestReady       = false;
    bool presentTiming         = false;
    bool presentAtAbsoluteTime = false;
    bool presentId2            = false;
};

class Context {
  public:
    Context() noexcept = default;
    ~Context() noexcept;

    Context(const Context&)                    = delete;
    auto operator=(const Context&) -> Context& = delete;

    Context(Context&& other) noexcept;
    auto operator=(Context&& other) noexcept -> Context&;

    [[nodiscard]] auto Instance() const noexcept -> InstanceView {
        return _instance;
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
    friend class ContextBuilder;

    std::optional<Vk::Instance> _ownedInstance;
    InstanceView               _instance;
    VkSurfaceKHR               _surface = VK_NULL_HANDLE;
    PhysicalDeviceInfo         _physical {};
    LogicalDevice              _device;
    DevicePresentSupport       _present {};
    EnabledFeatureSet          _enabledFeatures;
    bool                       _addressBindingReportEnabled = false;
};

class ContextBuilder {
  public:
    constexpr ContextBuilder() noexcept = default;

    constexpr auto ValidationMode(Vk::ValidationMode mode) noexcept -> ContextBuilder& {
        _validationMode = mode;
        return *this;
    }
    // Views and lvalue owners are borrowed; only an rvalue owner is adopted.
    auto Instance(InstanceView instance) noexcept -> ContextBuilder& {
        _ownedInstance.reset();
        _instance = instance;
        const Vk::Instance* const active = Vk::Instance::Active();
        _hasAddressBindingMessenger = active != nullptr && active->Handle() == instance.Handle() &&
                                     active->HasAddressBindingMessenger();
        return *this;
    }
    auto Instance(Vk::Instance&& instance) noexcept -> ContextBuilder& {
        _ownedInstance.emplace(std::move(instance));
        _instance                    = InstanceView(*_ownedInstance);
        _hasAddressBindingMessenger = _ownedInstance->HasAddressBindingMessenger();
        return *this;
    }
    constexpr auto Surface(VkSurfaceKHR surface) noexcept -> ContextBuilder& {
        _surface = surface;
        return *this;
    }
    constexpr auto PhysicalDevice(const PhysicalDeviceInfo& physical) noexcept -> ContextBuilder& {
        _physical = physical;
        return *this;
    }
    constexpr auto DeviceExtensions(std::span<const char* const> extensions) noexcept -> ContextBuilder& {
        _deviceExtensions.assign(extensions.begin(), extensions.end());
        return *this;
    }
    constexpr auto DeviceExtensions(const std::vector<const char*>& extensions) noexcept -> ContextBuilder& {
        _deviceExtensions.assign(extensions.begin(), extensions.end());
        return *this;
    }

    template <typename... Ts>
    auto DeviceFeatures(FeatureChain<Ts...>& chain) noexcept -> ContextBuilder& {
        _features        = chain.GetRoot();
        _enabledFeatures = chain.SnapshotEnabled();
        return *this;
    }

    constexpr auto ScoreFunction(DeviceScoreFunction score, const void* userdata = nullptr) noexcept -> ContextBuilder& {
        _scoreFn       = score;
        _scoreUserdata = userdata;
        return *this;
    }

    [[nodiscard]] auto SelectPhysicalDevice() noexcept -> std::expected<PhysicalDeviceInfo, Vk::Error>;
    [[nodiscard]] auto Build() noexcept -> std::expected<Context, Vk::Error>;

  private:
    std::optional<Vk::Instance>        _ownedInstance;
    InstanceView                       _instance;
    VkSurfaceKHR                       _surface = VK_NULL_HANDLE;
    bool                               _hasAddressBindingMessenger = false;
    PhysicalDeviceInfo                 _physical {};
    Vk::ValidationMode                 _validationMode = Vk::ValidationMode::On;
    std::vector<const char*>           _deviceExtensions;
    const VkPhysicalDeviceFeatures2*    _features = nullptr;
    EnabledFeatureSet                  _enabledFeatures;
    DeviceScoreFunction               _scoreFn       = nullptr;
    const void*                        _scoreUserdata = nullptr;
};

} // namespace ZHLN::Vk
