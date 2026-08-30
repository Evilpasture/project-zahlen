// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Instance.hpp

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>

namespace ZHLN::Vk {

// ============================================================================
// Vk::Instance — RAII owner of the Vulkan instance, its persistent debug
// messenger, and the validation/device-lost diagnostics.
//
// The C layer (RenderCore.c) is stateless: this class owns the counters that
// debug callbacks used to bump in C globals. The instance descriptor carries
// a ZHLN_DebugForwarding pointing back here, so both the pNext messenger
// (instance create/destroy coverage) and the persistent messenger (runtime
// coverage) funnel error severities into these atomics.
//
// The engine is single-instance by design. Active() exposes the live instance
// to engine-level diagnostics accessors (RenderContext statics); when an
// instance is retired, its counts are folded into process totals so
// before/after snapshots taken across an instance's full lifecycle (the test
// framework's suite wrapper) still observe its errors.
// ============================================================================
class Instance {
  public:
    Instance() noexcept = default;
    ~Instance() noexcept;

    Instance(const Instance&)                    = delete;
    auto operator=(const Instance&) -> Instance& = delete;

    Instance(Instance&& other) noexcept;
    auto operator=(Instance&& other) noexcept -> Instance&;

    // Creates the Vulkan instance (acquiring the loader through Volk) and,
    // when validation is enabled, the persistent debug messenger. Returns an
    // invalid Instance on failure (Valid() == false).
    [[nodiscard]] static auto
        Create(std::string_view appName, uint32_t appVersion, std::span<const std::string_view> extensions, ZHLN_ValidationMode validation) noexcept
            -> Instance;

    [[nodiscard]] auto Handle() const noexcept -> VkInstance {
        return _handle;
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _handle != VK_NULL_HANDLE;
    }

    // --- Process-wide diagnostics (active instance + retired totals) ------
    // Zero when no Vulkan instance has ever existed (CPU-only suites).

    [[nodiscard]] static auto ValidationErrorCount() noexcept -> uint32_t;
    [[nodiscard]] static auto DeviceLostCount() noexcept -> uint32_t;

    // Records a device-lost event observed by engine code (failed submits,
    // VK_ERROR_DEVICE_LOST returns). Bumps the active instance when there is
    // one, the retired totals otherwise (e.g. during teardown).
    static void NotifyDeviceLost() noexcept;

    // The single live instance, or null when none exists (engine is
    // single-instance by design).
    [[nodiscard]] static auto Active() noexcept -> Instance* {
        return _active.load(std::memory_order_acquire);
    }

  private:
    static void DebugHookTrampoline(void* userdata, VkDebugUtilsMessageSeverityFlagBitsEXT severity) noexcept;

    VkInstance                   _handle    = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT     _messenger = VK_NULL_HANDLE;
    ZHLN_DebugForwarding         _debugForwarding {};
    std::atomic<uint32_t>        _validationErrors {0};
    std::atomic<uint32_t>        _deviceLost {0};

    static std::atomic<Instance*> _active;
};

} // namespace ZHLN::Vk
