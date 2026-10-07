// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <string_view>

namespace ZHLN::Vk {

enum class ValidationMode : uint8_t {
    Off,
    On,
    GPU,
};

struct DiagnosticsSink {
    std::atomic<uint32_t>* validation = nullptr;
    std::atomic<uint32_t>* deviceLost  = nullptr;

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return validation != nullptr && deviceLost != nullptr;
    }
};

class Instance {
  public:
    Instance() noexcept;
    ~Instance() noexcept;

    Instance(const Instance&)                    = delete;
    auto operator=(const Instance&) -> Instance& = delete;

    Instance(Instance&& other) noexcept;
    auto operator=(Instance&& other) noexcept -> Instance&;

    [[nodiscard]] static auto Create(
        std::string_view appName,
        uint32_t appVersion,
        std::span<const std::string_view> extensions,
        ValidationMode validation
    ) noexcept -> Instance;

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
    static void IncrementNumericalDeviceLoss() noexcept;

    [[nodiscard]] static auto Active() noexcept -> Instance* {
        return _active.load(std::memory_order::acquire);
    }

  private:
    struct DebugState {
        Instance* owner = nullptr;
        bool      debugUtilsEnabled = false;
    };

    static auto VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
        void* userData
    ) noexcept -> VkBool32;

    void Destroy() noexcept;
    void RebindDebugState() noexcept;

    VkInstance               _handle                  = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT _messenger               = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT _addressBindingMessenger = VK_NULL_HANDLE;
    std::unique_ptr<DebugState> _debugState;

    std::atomic<uint32_t>  _validationErrors {0};
    std::atomic<uint32_t>  _deviceLost {0};
    std::atomic<uint32_t>* _validationTarget = &_validationErrors;
    std::atomic<uint32_t>* _deviceLostTarget = &_deviceLost;

    static std::atomic<Instance*>       _active;
    static std::atomic<DiagnosticsSink> _registeredSink;
};

} // namespace ZHLN::Vk
