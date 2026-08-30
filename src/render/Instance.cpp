// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Instance.cpp

#include "Instance.hpp"
#include "RenderCore.h"
#include "RenderCore.hpp" // ZHLN::Min (via RenderCore.inl -> Utils.hpp)

#include <cstring>
#include <utility>
#include <vector>

namespace ZHLN::Vk {

std::atomic<Instance*> Instance::_active {nullptr};

// Diagnostics observed by snapshots taken after the instance was retired
// (test suites create and destroy an engine inside a single wrapped test).
// Only touched on instance destruction, so plain atomics suffice.
namespace {
    std::atomic<uint32_t> g_retiredValidationErrors {0};
    std::atomic<uint32_t> g_retiredDeviceLost {0};
} // namespace

void Instance::DebugHookTrampoline(void* userdata, VkDebugUtilsMessageSeverityFlagBitsEXT severity) noexcept {
    auto& self = *static_cast<Instance*>(userdata);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        self._validationErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

Instance::~Instance() noexcept {
    if (_handle == VK_NULL_HANDLE) {
        return;
    }

    // Fold this instance's diagnostics into the process totals BEFORE
    // tearing anything down: snapshots taken afterwards (test suite wrappers)
    // must still observe errors that happened inside this instance's life.
    g_retiredValidationErrors.fetch_add(_validationErrors.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_retiredDeviceLost.fetch_add(_deviceLost.load(std::memory_order_relaxed), std::memory_order_relaxed);

    if (_messenger != VK_NULL_HANDLE) {
        ZHLN_DestroyDebugMessenger(_handle, _messenger);
        _messenger = VK_NULL_HANDLE;
    }

    vkDestroyInstance(_handle, nullptr);
    _handle = VK_NULL_HANDLE;

    Instance* expected = this;
    _active.compare_exchange_strong(expected, nullptr, std::memory_order_release, std::memory_order_relaxed);
}

Instance::Instance(Instance&& other) noexcept
    : _handle(std::exchange(other._handle, VK_NULL_HANDLE)), _messenger(std::exchange(other._messenger, VK_NULL_HANDLE)),
      _debugForwarding(other._debugForwarding), _validationErrors(other._validationErrors.load(std::memory_order_relaxed)),
      _deviceLost(other._deviceLost.load(std::memory_order_relaxed)) {
    // The C-side forwarding (and possibly _active) still points at the
    // moved-from address -- including the stack local in Create() when the
    // return wasn't elided. Re-point unconditionally: the hook is ours.
    if (_debugForwarding.hook != nullptr) {
        _debugForwarding.userdata = this;
    }
    if (_active.load(std::memory_order_acquire) == &other) {
        _active.store(this, std::memory_order_release);
    }
    other._debugForwarding = {};
    other._validationErrors.store(0, std::memory_order_relaxed);
    other._deviceLost.store(0, std::memory_order_relaxed);
}

auto Instance::operator=(Instance&& other) noexcept -> Instance& {
    if (this != &other) {
        // Retire ourselves exactly like the destructor, then take over.
        if (_handle != VK_NULL_HANDLE) {
            g_retiredValidationErrors.fetch_add(_validationErrors.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_retiredDeviceLost.fetch_add(_deviceLost.load(std::memory_order_relaxed), std::memory_order_relaxed);
            if (_messenger != VK_NULL_HANDLE) {
                ZHLN_DestroyDebugMessenger(_handle, _messenger);
            }
            vkDestroyInstance(_handle, nullptr);
            Instance* expected = this;
            _active.compare_exchange_strong(expected, &other, std::memory_order_release, std::memory_order_relaxed);
        }

        _handle             = std::exchange(other._handle, VK_NULL_HANDLE);
        _messenger          = std::exchange(other._messenger, VK_NULL_HANDLE);
        _debugForwarding    = other._debugForwarding;
        _validationErrors   = other._validationErrors.load(std::memory_order_relaxed);
        _deviceLost         = other._deviceLost.load(std::memory_order_relaxed);

        // See the move constructor: re-point the C-side forwarding at us.
        if (_debugForwarding.hook != nullptr) {
            _debugForwarding.userdata = this;
        }
        if (_active.load(std::memory_order_acquire) == &other) {
            _active.store(this, std::memory_order_release);
        }
        other._debugForwarding = {};
        other._validationErrors.store(0, std::memory_order_relaxed);
        other._deviceLost.store(0, std::memory_order_relaxed);
    }
    return *this;
}

auto Instance::Create(
    std::string_view appName, uint32_t appVersion, std::span<const std::string_view> extensions, ZHLN_ValidationMode validation
) noexcept -> Instance {
    Instance result;

    std::vector<const char*> cStrings;
    cStrings.reserve(extensions.size());
    for (const auto& extension: extensions) {
        cStrings.push_back(extension.data());
    }

    ZHLN_InstanceDesc desc = {
        .app_name        = {},
        .version         = appVersion,
        .extension_count = static_cast<uint32_t>(cStrings.size()),
        .severity_flags  = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .extensions      = cStrings.data(),
        .validation_mode = validation,
        .debug           = &result._debugForwarding,
    };

    const size_t copySize = ZHLN::Min(appName.size(), sizeof(desc.app_name) - 1);
    std::memcpy(desc.app_name, appName.data(), copySize);
    desc.app_name[copySize] = '\0';

    result._debugForwarding = {.hook = &Instance::DebugHookTrampoline, .userdata = &result};
    // From here the pNext messenger can fire into result's counters --
    // including during vkCreateInstance itself.

    result._handle = ZHLN_CreateInstance(&desc);
    if (result._handle == VK_NULL_HANDLE) {
        result._debugForwarding = {};
        return result;
    }

    // Persistent messenger: the pNext one only covers instance create/destroy.
    // Errors AND warnings: a warning the engine cannot explain is a warning
    // worth fixing at the source, not filtering here.
    if (validation != ZHLN_VALIDATION_OFF) {
        result._messenger = ZHLN_CreateDebugMessenger(
            result._handle, VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, &result._debugForwarding
        );
    }

    _active.store(&result, std::memory_order_release);
    return result;
}

auto Instance::ValidationErrorCount() noexcept -> uint32_t {
    const Instance* const active = _active.load(std::memory_order_acquire);
    return g_retiredValidationErrors.load(std::memory_order_relaxed) + (active != nullptr ? active->_validationErrors.load(std::memory_order_relaxed) : 0);
}

auto Instance::DeviceLostCount() noexcept -> uint32_t {
    const Instance* const active = _active.load(std::memory_order_acquire);
    return g_retiredDeviceLost.load(std::memory_order_relaxed) + (active != nullptr ? active->_deviceLost.load(std::memory_order_relaxed) : 0);
}

void Instance::NotifyDeviceLost() noexcept {
    if (Instance* const active = _active.load(std::memory_order_acquire); active != nullptr) {
        active->_deviceLost.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_retiredDeviceLost.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace ZHLN::Vk
