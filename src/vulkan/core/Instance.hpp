// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <type_traits>

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

class Instance;

class InstanceView {
  public:
    constexpr InstanceView() noexcept = default;
    explicit constexpr InstanceView(VkInstance handle, bool debugUtilsEnabled = false) noexcept:
        _handle(handle), _debugUtilsEnabled(debugUtilsEnabled) {
    }
    InstanceView(const Instance& instance) noexcept;

    [[nodiscard]] constexpr auto Handle() const noexcept -> VkInstance {
        return _handle;
    }
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _handle != VK_NULL_HANDLE;
    }
    [[nodiscard]] constexpr auto DebugUtilsEnabled() const noexcept -> bool {
        return _debugUtilsEnabled;
    }
    explicit constexpr operator bool() const noexcept {
        return Valid();
    }

  private:
    VkInstance _handle            = VK_NULL_HANDLE;
    bool       _debugUtilsEnabled = false;
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

    [[nodiscard]] static auto Create(
        std::string_view                  appName    = "ZHLN Engine",
        uint32_t                          appVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
        std::span<const std::string_view> extensions = {},
        ValidationMode                    validation = ValidationMode::On
    ) noexcept -> std::expected<Instance, ErrorCode>;

    [[nodiscard]] auto Handle() const noexcept -> VkInstance {
        return _handle;
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _handle != VK_NULL_HANDLE;
    }
    [[nodiscard]] auto DebugUtilsEnabled() const noexcept -> bool {
        return _debugUtilsEnabled;
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

  private:
    static auto VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
        VkDebugUtilsMessageTypeFlagsEXT             type,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
        void*                                       userData
    ) noexcept -> VkBool32;

    void Destroy() noexcept;

    VkInstance               _handle             = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT _messenger          = VK_NULL_HANDLE;
    bool                     _debugUtilsEnabled  = false;
    bool                     _ownsGlobalDispatch = false;
};

inline InstanceView::InstanceView(const Instance& instance) noexcept:
    _handle(instance.Handle()), _debugUtilsEnabled(instance.DebugUtilsEnabled()) {
}

} // namespace ZHLN::Vk
