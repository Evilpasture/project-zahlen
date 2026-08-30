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
std::atomic<DiagnosticsSink> Instance::_registeredSink {DiagnosticsSink {}};

void Instance::UseDiagnostics(DiagnosticsSink sink) noexcept {
    // Both or neither: a half-registered sink would split one logical
    // diagnostics session across two storages.
    if (!sink.Valid()) {
        sink = {};
    }
    _registeredSink.store(sink, std::memory_order::release);
}

void Instance::DebugHookTrampoline(void* userdata, VkDebugUtilsMessageSeverityFlagBitsEXT severity) noexcept {
    auto& self = *static_cast<Instance*>(userdata);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        // Target is the caller's registered sink or this instance's own
        // member -- resolved once at Create() and immutable afterwards.
        self._validationTarget->fetch_add(1, std::memory_order::relaxed);
    }
}

Instance::~Instance() noexcept {
    if (_handle == VK_NULL_HANDLE) {
        return;
    }

    // No retirement fold: with a registered sink, every increment this
    // instance ever made (including teardown-time callbacks still to come)
    // already went into the caller's storage, which outlives us. Without
    // one, the live counts die with the instance by design.

    if (_messenger != VK_NULL_HANDLE) {
        ZHLN_DestroyDebugMessenger(_handle, _messenger);
        _messenger = VK_NULL_HANDLE;
    }

    vkDestroyInstance(_handle, nullptr);
    _handle = VK_NULL_HANDLE;

    Instance* expected = this;
    _active.compare_exchange_strong(expected, nullptr, std::memory_order::release, std::memory_order::relaxed);
}

Instance::Instance(Instance&& other) noexcept
    : _handle(std::exchange(other._handle, VK_NULL_HANDLE)), _messenger(std::exchange(other._messenger, VK_NULL_HANDLE)),
      _debugForwarding(std::move(other._debugForwarding)), _validationErrors(other._validationErrors.load(std::memory_order::relaxed)),
      _deviceLost(other._deviceLost.load(std::memory_order::relaxed)),
      _validationTarget(other._validationTarget == &other._validationErrors ? &_validationErrors : other._validationTarget),
      _deviceLostTarget(other._deviceLostTarget == &other._deviceLost ? &_deviceLost : other._deviceLostTarget) {
    // Vulkan stores the forwarding object pointer itself as pUserData, so the
    // pointee must be stable across moves; only the owning Instance* inside it
    // needs rebinding.
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
        // Retire ourselves exactly like the destructor, then take over.
        if (_handle != VK_NULL_HANDLE) {
            if (_messenger != VK_NULL_HANDLE) {
                ZHLN_DestroyDebugMessenger(_handle, _messenger);
            }
            vkDestroyInstance(_handle, nullptr);
            Instance* expected = this;
            _active.compare_exchange_strong(expected, &other, std::memory_order::release, std::memory_order::relaxed);
        }

        _handle           = std::exchange(other._handle, VK_NULL_HANDLE);
        _messenger        = std::exchange(other._messenger, VK_NULL_HANDLE);
        _debugForwarding  = std::move(other._debugForwarding);
        _validationErrors = other._validationErrors.load(std::memory_order::relaxed);
        _deviceLost       = other._deviceLost.load(std::memory_order::relaxed);
        _validationTarget = other._validationTarget == &other._validationErrors ? &_validationErrors : other._validationTarget;
        _deviceLostTarget = other._deviceLostTarget == &other._deviceLost ? &_deviceLost : other._deviceLostTarget;

        // See the move constructor: the forwarding object's address stays
        // stable; only its owning Instance* needs rebinding.
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

auto Instance::Create(
    std::string_view appName, uint32_t appVersion, std::span<const std::string_view> extensions, ZHLN_ValidationMode validation
) noexcept -> Instance {
    Instance result;

    // Resolve the counting target before anything can fire: the pNext
    // messenger delivers callbacks during vkCreateInstance itself.
    const DiagnosticsSink sink = _registeredSink.load(std::memory_order::acquire);
    if (sink.Valid()) {
        result._validationTarget = sink.validation;
        result._deviceLostTarget = sink.deviceLost;
    }

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
        .debug           = result._debugForwarding.get(),
    };

    const size_t copySize = ZHLN::Min(appName.size(), sizeof(desc.app_name) - 1);
    std::memcpy(desc.app_name, appName.data(), copySize);
    desc.app_name[copySize] = '\0';

    if (result._debugForwarding == nullptr) {
        return result;
    }
    *result._debugForwarding = {.hook = &Instance::DebugHookTrampoline, .userdata = &result};
    // From here the pNext messenger can fire into result's counters -- including during vkCreateInstance itself.

    result._handle = ZHLN_CreateInstance(&desc);
    if (result._handle == VK_NULL_HANDLE) {
        *result._debugForwarding = {};
        return result;
    }

    // Persistent messenger: the pNext one only covers instance create/destroy.
    // Errors AND warnings: a warning the engine cannot explain is a warning
    // worth fixing at the source, not filtering here.
    if (validation != ZHLN_VALIDATION_OFF) {
        result._messenger = ZHLN_CreateDebugMessenger(
            result._handle, VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, result._debugForwarding.get()
        );
    }

    // Claim the single live-instance slot. volk's dispatch tables are
    // process-global, so two live instances cannot be served; refuse loudly
    // instead of letting a second instance silently steal the slot (which
    // would re-route the first one's notifications and break its retirement).
    Instance* claimed = nullptr;
    if (!_active.compare_exchange_strong(claimed, &result, std::memory_order::release, std::memory_order::relaxed)) {
        if (result._messenger != VK_NULL_HANDLE) {
            ZHLN_DestroyDebugMessenger(result._handle, result._messenger);
        }
        vkDestroyInstance(result._handle, nullptr);
        result._handle             = VK_NULL_HANDLE;
        result._messenger          = VK_NULL_HANDLE;
        result._validationTarget   = &result._validationErrors;
        result._deviceLostTarget   = &result._deviceLost;
        *result._debugForwarding   = {};
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

void Instance::NotifyDeviceLost() noexcept {
    if (Instance* const active = _active.load(std::memory_order::acquire); active != nullptr) {
        active->_deviceLostTarget->fetch_add(1, std::memory_order::relaxed);
    }
    // No live instance: unobservable by design. Observers bracketing engine
    // lifetimes hold a registered sink; one that dies with no instance live
    // never happened as far as any reader can tell.
}

} // namespace ZHLN::Vk
