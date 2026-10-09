// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../diagnostics/GPUAddressTracker.hpp"
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Render/Diagnostics.hpp>
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN::Vk {

enum class ValidationMode : uint8_t {
    Off,
    On,
    GPU,
};

enum class InstanceError : uint8_t {
    CreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Vulkan instance creation failed"> {}) = 1,
    GlobalDispatchInUse ZHLN_ANNOTATION(ZHLN::Description<"Volk global dispatch is still in use by another Vulkan instance"> {}) = 2,
};

// Stable heap-owned callback state. Vulkan receives this directly through
// pUserData; it has no back-pointer to a movable Instance and contains all
// per-instance diagnostics/tracking state.
struct InstanceDiagnostics {
    std::atomic<uint32_t>  localValidationErrors {0};
    std::atomic<uint32_t>  localDeviceLost {0};
    std::atomic<uint32_t>* validationTarget = &localValidationErrors;
    std::atomic<uint32_t>* deviceLostTarget = &localDeviceLost;
    GPUAddressTracker     addressTracker;
    bool                  debugUtilsEnabled = false;
    bool                  hasAddressBindingMessenger = false;
};

class Instance;

// A non-owning view of a Vulkan instance. Wrapping a raw handle is deliberately
// explicit; a view made from an Instance also carries its stable diagnostics
// pointer, which remains valid across moves of the owning Instance.
class InstanceView {
  public:
    constexpr InstanceView() noexcept = default;
    explicit constexpr InstanceView(VkInstance handle) noexcept: _handle(handle) {
    }
    InstanceView(const Instance& instance) noexcept;

    [[nodiscard]] constexpr auto Handle() const noexcept -> VkInstance {
        return _handle;
    }
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _handle != VK_NULL_HANDLE;
    }
    explicit constexpr operator bool() const noexcept {
        return Valid();
    }

    [[nodiscard]] auto ValidationErrorCount() const noexcept -> uint32_t;
    [[nodiscard]] auto DeviceLostCount() const noexcept -> uint32_t;
    void IncrementDeviceLost() const noexcept;
    [[nodiscard]] auto AddressTracker() const noexcept -> GPUAddressTracker*;
    [[nodiscard]] auto HasAddressBindingMessenger() const noexcept -> bool;

  private:
    friend class Instance;
    VkInstance           _handle      = VK_NULL_HANDLE;
    InstanceDiagnostics* _diagnostics = nullptr;
};

static_assert(std::is_trivially_copyable_v<InstanceView>);

class Instance {
  public:
    Instance() noexcept;
    ~Instance() noexcept;

    Instance(const Instance&)                    = delete;
    auto operator=(const Instance&) -> Instance& = delete;

    Instance(Instance&& other) noexcept;
    auto operator=(Instance&& other) noexcept -> Instance&;

    [[nodiscard]] auto Handle() const noexcept -> VkInstance {
        return _handle;
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _handle != VK_NULL_HANDLE;
    }
    [[nodiscard]] auto HasAddressBindingMessenger() const noexcept -> bool {
        return _diagnostics != nullptr && _diagnostics->hasAddressBindingMessenger;
    }
    [[nodiscard]] auto ValidationErrorCount() const noexcept -> uint32_t;
    [[nodiscard]] auto DeviceLostCount() const noexcept -> uint32_t;
    void IncrementDeviceLost() noexcept;
    [[nodiscard]] auto AddressTracker() noexcept -> GPUAddressTracker& {
        return _diagnostics->addressTracker;
    }

  private:
    friend class InstanceBuilder;
    friend class InstanceView;

    [[nodiscard]] static auto Create(
        std::string_view appName,
        uint32_t appVersion,
        std::span<const std::string_view> extensions,
        ValidationMode validation,
        DiagnosticsSink* diagnostics
    ) noexcept -> std::expected<Instance, ErrorCode>;

    static auto VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
        VkDebugUtilsMessageTypeFlagsEXT             type,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
        void*                                       userData
    ) noexcept -> VkBool32;

    void Destroy() noexcept;

    VkInstance                       _handle                  = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT         _messenger               = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT         _addressBindingMessenger = VK_NULL_HANDLE;
    std::unique_ptr<InstanceDiagnostics> _diagnostics;
    bool                              _ownsGlobalDispatch = false;
};

inline InstanceView::InstanceView(const Instance& instance) noexcept:
    _handle(instance.Handle()), _diagnostics(instance._diagnostics.get()) {
}

inline auto InstanceView::ValidationErrorCount() const noexcept -> uint32_t {
    return _diagnostics != nullptr ? _diagnostics->validationTarget->load(std::memory_order::relaxed) : 0;
}

inline auto InstanceView::DeviceLostCount() const noexcept -> uint32_t {
    return _diagnostics != nullptr ? _diagnostics->deviceLostTarget->load(std::memory_order::relaxed) : 0;
}

inline void InstanceView::IncrementDeviceLost() const noexcept {
    if (_diagnostics != nullptr) {
        _diagnostics->deviceLostTarget->fetch_add(1, std::memory_order::relaxed);
    }
}

inline auto InstanceView::AddressTracker() const noexcept -> GPUAddressTracker* {
    return _diagnostics != nullptr ? &_diagnostics->addressTracker : nullptr;
}

inline auto InstanceView::HasAddressBindingMessenger() const noexcept -> bool {
    return _diagnostics != nullptr && _diagnostics->hasAddressBindingMessenger;
}

class InstanceBuilder {
  public:
    constexpr InstanceBuilder() noexcept = default;

    constexpr auto AppName(std::string_view name) noexcept -> InstanceBuilder& {
        _appName = name;
        return *this;
    }
    constexpr auto AppVersion(uint32_t version) noexcept -> InstanceBuilder& {
        _appVersion = version;
        return *this;
    }
    constexpr auto ValidationMode(Vk::ValidationMode mode) noexcept -> InstanceBuilder& {
        _validationMode = mode;
        return *this;
    }
    constexpr auto Extensions(std::span<const std::string_view> extensions) noexcept -> InstanceBuilder& {
        _extensions.assign(extensions.begin(), extensions.end());
        return *this;
    }
    constexpr auto Diagnostics(DiagnosticsSink* diagnostics) noexcept -> InstanceBuilder& {
        _diagnostics = diagnostics;
        return *this;
    }

    [[nodiscard]] auto Build() noexcept -> std::expected<Instance, ErrorCode>;

  private:
    std::string_view              _appName        = "ZHLN Engine";
    uint32_t                      _appVersion     = VK_MAKE_API_VERSION(0, 1, 0, 0);
    Vk::ValidationMode            _validationMode = Vk::ValidationMode::On;
    std::vector<std::string_view> _extensions;
    DiagnosticsSink*              _diagnostics = nullptr;
};

} // namespace ZHLN::Vk
