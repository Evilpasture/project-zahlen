// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN::Vk {

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
        return _instance;
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
    [[nodiscard]] auto Physical() const noexcept -> VkPhysicalDevice {
        return _physical.handle;
    }
    [[nodiscard]] auto PhysicalInfo() const noexcept -> const ZHLN_PhysicalDeviceInfo& {
        return _physical;
    }

    [[nodiscard("Always verify context initialization; check Valid() before use")]]
    auto Valid() const noexcept -> bool {
        return _device.handle != VK_NULL_HANDLE;
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

  private:
    VkInstance              _instance = VK_NULL_HANDLE;
    VkSurfaceKHR            _surface  = VK_NULL_HANDLE;
    ZHLN_PhysicalDeviceInfo _physical = {};
    ZHLN_Device             _device   = {};
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

    constexpr Builder& EnableValidation(bool enable) noexcept {
        _enableValidation = enable;
        return *this;
    }

    constexpr Builder& Instance(VkInstance inst) noexcept {
        _instance = inst;
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

    // Support standard spans
    constexpr Builder& DeviceExtensions(std::span<const char* const> exts) noexcept {
        _deviceExtensions.assign(exts.begin(), exts.end());
        return *this;
    }

    // Direct overload to resolve single-step implicit conversions from ExtensionResult
    constexpr Builder& DeviceExtensions(const std::vector<const char*>& exts) noexcept {
        _deviceExtensions.assign(exts.begin(), exts.end());
        return *this;
    }

    constexpr Builder& DeviceFeatures(const VkPhysicalDeviceFeatures2* features) noexcept {
        _features = features;
        return *this;
    }

    constexpr Builder& ScoreFunction(ZHLN_DeviceScoreFn scoreFn, void* userdata = nullptr) noexcept {
        _scoreFn       = scoreFn;
        _scoreUserdata = userdata;
        return *this;
    }

    // --- Build Steps ---
    [[nodiscard]] std::expected<VkInstance, ZHLN::Error>              BuildInstance() const noexcept;
    [[nodiscard]] std::expected<ZHLN_PhysicalDeviceInfo, ZHLN::Error> SelectPhysicalDevice() const noexcept;
    [[nodiscard]] std::expected<Context, ZHLN::Error>                 Build() const noexcept;

  private:
    std::string_view _appName          = "ZHLN Engine";
    uint32_t         _appVersion       = VK_MAKE_API_VERSION(0, 1, 0, 0);
    bool             _enableValidation = true;

    VkInstance              _instance = VK_NULL_HANDLE;
    VkSurfaceKHR            _surface  = VK_NULL_HANDLE;
    ZHLN_PhysicalDeviceInfo _physical = {};

    std::vector<std::string_view>    _instanceExtensions;
    std::vector<const char*>         _deviceExtensions;
    const VkPhysicalDeviceFeatures2* _features      = nullptr;
    ZHLN_DeviceScoreFn               _scoreFn       = nullptr;
    void*                            _scoreUserdata = nullptr;
};

} // namespace ZHLN::Vk
