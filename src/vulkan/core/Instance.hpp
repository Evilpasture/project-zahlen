// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <VkError.hpp>
#include <Zahlen/Core/Description.hpp>
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
};

struct DiagnosticsSink {
    std::atomic<uint32_t>* validation = nullptr;
    std::atomic<uint32_t>* deviceLost = nullptr;

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return validation != nullptr && deviceLost != nullptr;
    }
};

class Instance;

// A non-owning view of a Vulkan instance. Wrapping a raw handle is deliberately
// explicit; a view made from an Instance is an ordinary, non-owning conversion.
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

  private:
    VkInstance _handle = VK_NULL_HANDLE;
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

    static void UseDiagnostics(DiagnosticsSink sink) noexcept;

    [[nodiscard]] auto Handle() const noexcept -> VkInstance {
        return _handle;
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _handle != VK_NULL_HANDLE;
    }
    [[nodiscard]] auto HasAddressBindingMessenger() const noexcept -> bool {
        return _addressBindingMessenger != VK_NULL_HANDLE;
    }

    [[nodiscard]] static auto ValidationErrorCount() noexcept -> uint32_t;
    [[nodiscard]] static auto DeviceLostCount() noexcept -> uint32_t;

    // Be fucking warned, this does nothing. The only purpose here is to add one. That's it. It won't magically rebuild the device for you.
    static void IncrementNumericalDeviceLoss() noexcept;

    [[nodiscard]] static auto Active() noexcept -> Instance* {
        return s_active.load(std::memory_order::acquire);
    }

  private:
    friend class InstanceBuilder;

    [[nodiscard]] static auto
        Create(std::string_view appName, uint32_t appVersion, std::span<const std::string_view> extensions, ValidationMode validation) noexcept
        -> std::expected<Vk::Instance, Vk::Error>;

    struct DebugState {
        // Non-owning back-pointer: the Instance owns this DebugState (see
        // _debugState), and the Vulkan debug callback receives it as userData,
        // reaching the Instance through owner. It is deliberately a raw pointer,
        // not a reference: the owning Instance is movable, and RebindDebugState
        // repoints owner after a move.
        Instance* owner             = nullptr;
        bool      debugUtilsEnabled = false;
    };

    static auto VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
        VkDebugUtilsMessageTypeFlagsEXT             type,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
        void*                                       userData
    ) noexcept -> VkBool32;

    void Destroy() noexcept;
    void RebindDebugState() noexcept;

    VkInstance                  _handle                  = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT    _messenger               = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT    _addressBindingMessenger = VK_NULL_HANDLE;
    std::unique_ptr<DebugState> _debugState;

    std::atomic<uint32_t>  _validationErrors {0};
    std::atomic<uint32_t>  _deviceLost {0};
    std::atomic<uint32_t>* _validationTarget = &_validationErrors;
    std::atomic<uint32_t>* _deviceLostTarget = &_deviceLost;

    static std::atomic<Instance*>       s_active;
    static std::atomic<DiagnosticsSink> s_registered_sink;
};

inline InstanceView::InstanceView(const Instance& instance) noexcept: _handle(instance.Handle()) {
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

    [[nodiscard]] auto Build() noexcept -> std::expected<Instance, Vk::Error>;

  private:
    std::string_view              _appName        = "ZHLN Engine";
    uint32_t                      _appVersion     = VK_MAKE_API_VERSION(0, 1, 0, 0);
    Vk::ValidationMode            _validationMode = Vk::ValidationMode::On;
    std::vector<std::string_view> _extensions;
};

} // namespace ZHLN::Vk
