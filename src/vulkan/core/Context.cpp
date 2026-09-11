// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Context.hpp"
#include "RenderCore.h"
#include "RenderCore.hpp"

namespace ZHLN::Vk {

ZHLN_PhysicalDeviceInfo SelectDevice(VkInstance instance, VkSurfaceKHR surface) noexcept {
    ZHLN_DeviceSelectDesc select_desc = {.instance = instance, .surface = surface, .score_fn = nullptr, .score_userdata = nullptr};
    return ZHLN_SelectPhysicalDevice(&select_desc);
}

// ============================================================================
// Context Implementation
// ============================================================================

Context::~Context() noexcept {
    if (_device.handle != VK_NULL_HANDLE) {
        vkDestroyDevice(_device.handle, nullptr);
    }
    // _instanceObject's destructor tears down the persistent debug messenger
    // (if any) and then the instance, in that order, folding its diagnostics
    // into the process totals.
}

Context::Context(Context&& other) noexcept:
    _instanceObject(std::move(other._instanceObject)), _surface(std::exchange(other._surface, VK_NULL_HANDLE)),
    _physical(std::exchange(other._physical, {})), _device(std::exchange(other._device, {})) {
}

auto Context::operator=(Context&& other) noexcept -> Context& {
    if (this != &other) {
        if (_device.handle != VK_NULL_HANDLE) {
            vkDestroyDevice(_device.handle, nullptr);
        }
        // Instance::operator= retires our instance (messenger first) and
        // re-points the diagnostics forwarding at this object.
        _instanceObject = std::move(other._instanceObject);
        _surface        = std::exchange(other._surface, VK_NULL_HANDLE);
        _physical       = std::exchange(other._physical, {});
        _device         = std::exchange(other._device, {});
    }
    return *this;
}

// ============================================================================
// Builder Implementation
// ============================================================================

std::expected<Vk::Instance, ZHLN::Error> Context::Builder::BuildInstance() noexcept {
    // Ownership leaves with the return value: the caller must keep the
    // Vk::Instance alive and hand it back via Instance(Vk::Instance&&)
    // before Build(). A builder that still owns an instance when it dies
    // destroys it (RAII -- failed bring-ups cannot leak).
    _instanceObject = Instance::Create(_appName, _appVersion, _instanceExtensions, _validationMode);
    if (!_instanceObject.Valid()) {
        return std::unexpected(ContextError::InstanceCreationFailed);
    }
    _instanceView = _instanceObject.Handle();
    return std::move(_instanceObject);
}

std::expected<ZHLN_PhysicalDeviceInfo, ZHLN::Error> Context::Builder::SelectPhysicalDevice() const noexcept {
    const VkInstance        view = _instanceView != VK_NULL_HANDLE ? _instanceView : _instanceObject.Handle();
    ZHLN_DeviceSelectDesc   select_desc = {.instance = view, .surface = _surface, .score_fn = _scoreFn, .score_userdata = _scoreUserdata};
    ZHLN_PhysicalDeviceInfo info        = ZHLN_SelectPhysicalDevice(&select_desc);
    if (info.handle == VK_NULL_HANDLE) {
        return std::unexpected(ContextError::NoSuitableDeviceFound);
    }
    return info;
}

std::expected<Context, Error> Context::Builder::Build() noexcept {
    Context ctx;
    ctx._surface  = _surface;
    ctx._physical = _physical;

    const ZHLN_DeviceDesc device_desc = {
        .physical          = &ctx._physical,
        .extensions        = _deviceExtensions.data(),
        .extension_count   = static_cast<uint32_t>(_deviceExtensions.size()),
        .features          = _features,
        .enable_validation = (_validationMode != ZHLN_VALIDATION_OFF),
    };

    ctx._device = ZHLN_CreateDevice(&device_desc);
    if (ctx._device.handle == VK_NULL_HANDLE) {
        return std::unexpected(ContextError::DeviceCreationFailed);
    }

    // Only take ownership of the instance once device creation succeeds.
    // The persistent debug messenger already exists: Instance::Create set it
    // up (errors AND warnings -- a warning the engine cannot explain is a
    // warning worth fixing at the source) and owns its teardown.
    if (!_instanceObject.Valid()) {
        return std::unexpected(ContextError::InstanceCreationFailed);
    }
    ctx._instanceObject = std::move(_instanceObject);

    return ctx;
}

} // namespace ZHLN::Vk
