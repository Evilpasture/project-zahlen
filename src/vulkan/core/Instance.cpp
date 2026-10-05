// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "Instance.hpp"
#include "RenderCore.h"
#include "RenderCore.hpp"
#include "../diagnostics/GPUAddressTracker.hpp"
#include <Zahlen/Core/Math.hpp>
#include <cstring>
#include <utility>
#include <vector>

namespace ZHLN::Vk {

std::atomic<Instance*>       Instance::_active {nullptr};
std::atomic<DiagnosticsSink> Instance::_registeredSink {DiagnosticsSink {}};

void Instance::UseDiagnostics(DiagnosticsSink sink) noexcept {
    if (!sink.Valid()) {
        sink = {};
    }
    _registeredSink.store(sink, std::memory_order::release);
}

void Instance::DebugHookTrampoline(void* userdata, VkDebugUtilsMessageSeverityFlagBitsEXT severity) noexcept {
    auto& self = *static_cast<Instance*>(userdata);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        self._validationTarget->fetch_add(1, std::memory_order::relaxed);
    }
}

void Instance::DeviceAddressBindingHookTrampoline(
    [[maybe_unused]] void* userdata,
    const VkDeviceAddressBindingCallbackDataEXT* binding,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData
) noexcept {
    if (binding != nullptr) {
        GPUAddressTracker::Get().OnBindingEvent(*binding, callbackData);
    }
}

Instance::~Instance() noexcept {
    if (_handle == nullptr) {
        return;
    }


    if (_addressBindingMessenger != nullptr) {
        ZHLN_DestroyDebugMessenger(_handle, _addressBindingMessenger);
        _addressBindingMessenger = nullptr;
    }
    if (_messenger != nullptr) {
        ZHLN_DestroyDebugMessenger(_handle, _messenger);
        _messenger = nullptr;
    }

    vkDestroyInstance(_handle, nullptr);
    _handle = nullptr;

    Instance* expected = this;
    _active.compare_exchange_strong(expected, nullptr, std::memory_order::release, std::memory_order::relaxed);
}

Instance::Instance(Instance&& other) noexcept:
    _handle(std::exchange(other._handle, nullptr)), _messenger(std::exchange(other._messenger, nullptr)),
    _addressBindingMessenger(std::exchange(other._addressBindingMessenger, nullptr)), _debugForwarding(std::move(other._debugForwarding)),
    _validationErrors(other._validationErrors.load(std::memory_order::relaxed)), _deviceLost(other._deviceLost.load(std::memory_order::relaxed)),
    _validationTarget(other._validationTarget == &other._validationErrors ? &_validationErrors : other._validationTarget),
    _deviceLostTarget(other._deviceLostTarget == &other._deviceLost ? &_deviceLost : other._deviceLostTarget) {
    if (_debugForwarding && _debugForwarding->hook != nullptr) {
        _debugForwarding->userdata = this;
    }
    if (_active.load(std::memory_order::acquire) == &other) {
        _active.store(this, std::memory_order::release);
    }
    other._validationErrors.store(0, std::memory_order::relaxed);
    other._deviceLost.store(0, std::memory_order::relaxed);
    other._validationTarget = &other._validationErrors;
    other._deviceLostTarget = &other._deviceLost;
}

auto Instance::operator=(Instance&& other) noexcept -> Instance& {
    if (this != &other) {
        if (_handle != nullptr) {
            if (_addressBindingMessenger != nullptr) {
                ZHLN_DestroyDebugMessenger(_handle, _addressBindingMessenger);
            }
            if (_messenger != nullptr) {
                ZHLN_DestroyDebugMessenger(_handle, _messenger);
            }
            vkDestroyInstance(_handle, nullptr);
            Instance* expected = this;
            _active.compare_exchange_strong(expected, &other, std::memory_order::release, std::memory_order::relaxed);
        }

        _handle                  = std::exchange(other._handle, nullptr);
        _messenger               = std::exchange(other._messenger, nullptr);
        _addressBindingMessenger = std::exchange(other._addressBindingMessenger, nullptr);
        _debugForwarding         = std::move(other._debugForwarding);
        _validationErrors = other._validationErrors.load(std::memory_order::relaxed);
        _deviceLost       = other._deviceLost.load(std::memory_order::relaxed);
        _validationTarget = other._validationTarget == &other._validationErrors ? &_validationErrors : other._validationTarget;
        _deviceLostTarget = other._deviceLostTarget == &other._deviceLost ? &_deviceLost : other._deviceLostTarget;

        if (_debugForwarding && _debugForwarding->hook != nullptr) {
            _debugForwarding->userdata = this;
        }
        if (_active.load(std::memory_order::acquire) == &other) {
            _active.store(this, std::memory_order::release);
        }
        other._validationErrors.store(0, std::memory_order::relaxed);
        other._deviceLost.store(0, std::memory_order::relaxed);
        other._validationTarget = &other._validationErrors;
        other._deviceLostTarget = &other._deviceLost;
    }
    return *this;
}

auto Instance::Create(std::string_view appName, uint32_t appVersion, std::span<const std::string_view> extensions, ZHLN_ValidationMode validation) noexcept
    -> Instance {
    Instance result;

    const DiagnosticsSink sink = _registeredSink.load(std::memory_order::acquire);
    if (sink.Valid()) {
        result._validationTarget = sink.validation;
        result._deviceLostTarget = sink.deviceLost;
    }

    std::vector<const char*> c_strings;
    c_strings.reserve(extensions.size());
    for (const auto& extension: extensions) {
        c_strings.push_back(extension.data());
    }

    ZHLN_InstanceDesc desc = {
        .app_name        = {},
        .version         = appVersion,
        .extension_count = static_cast<uint32_t>(c_strings.size()),
        .severity_flags  = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .extensions      = c_strings.data(),
        .validation_mode = validation,
        .debug           = result._debugForwarding.get(),
    };

    const size_t copy_size = ZHLN::Math::Min(appName.size(), sizeof(desc.app_name) - 1);
    std::memcpy(desc.app_name, appName.data(), copy_size);
    desc.app_name[copy_size] = '\0';

    if (result._debugForwarding == nullptr) {
        return result;
    }
    *result._debugForwarding = {
        .hook                        = &Instance::DebugHookTrampoline,
        .device_address_binding_hook = &Instance::DeviceAddressBindingHookTrampoline,
        .userdata                    = &result,
    };

    result._handle = ZHLN_CreateInstance(&desc);
    if (result._handle == nullptr) {
        *result._debugForwarding = {};
        return result;
    }

    if (validation != ZHLN_VALIDATION_OFF && result._debugForwarding->debug_utils_enabled) {
        result._messenger = ZHLN_CreateDebugMessenger(
            result._handle, VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, result._debugForwarding.get()
        );
        result._addressBindingMessenger = ZHLN_CreateDeviceAddressBindingMessenger(result._handle, result._debugForwarding.get());
    }

    Instance* claimed = nullptr;
    if (!_active.compare_exchange_strong(claimed, &result, std::memory_order::release, std::memory_order::relaxed)) {
        if (result._addressBindingMessenger != nullptr) {
            ZHLN_DestroyDebugMessenger(result._handle, result._addressBindingMessenger);
        }
        if (result._messenger != nullptr) {
            ZHLN_DestroyDebugMessenger(result._handle, result._messenger);
        }
        vkDestroyInstance(result._handle, nullptr);
        result._handle                  = nullptr;
        result._messenger               = nullptr;
        result._addressBindingMessenger = nullptr;
        result._validationTarget = &result._validationErrors;
        result._deviceLostTarget = &result._deviceLost;
        *result._debugForwarding = {};
        return result;
    }
    return result;
}

auto Instance::ValidationErrorCount() noexcept -> uint32_t {
    const Instance* const active = _active.load(std::memory_order::acquire);
    return active != nullptr ? active->_validationTarget->load(std::memory_order::relaxed) : 0;
}

auto Instance::DeviceLostCount() noexcept -> uint32_t {
    const Instance* const active = _active.load(std::memory_order::acquire);
    return active != nullptr ? active->_deviceLostTarget->load(std::memory_order::relaxed) : 0;
}

void Instance::IncrementNumericalDeviceLoss() noexcept {
    if (Instance* const active = _active.load(std::memory_order::acquire); active != nullptr) {
        active->_deviceLostTarget->fetch_add(1, std::memory_order::relaxed);
    }
}

}
