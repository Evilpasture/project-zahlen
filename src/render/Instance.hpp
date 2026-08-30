// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Instance.hpp

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <string_view>

namespace ZHLN::Vk {

// ============================================================================
// Caller-owned storage for render diagnostics. An observer that needs
// diagnostic values to OUTLIVE an engine (e.g. a test framework bracketing
// whole engine lifecycles) registers its sink via Instance::UseDiagnostics()
// before creating engines; every instance created afterwards increments these
// atomics directly -- including teardown-time events fired while the instance
// is being destroyed. The library holds no post-mortem state of its own.
// ============================================================================
struct DiagnosticsSink {
    std::atomic<uint32_t>* validation = nullptr;
    std::atomic<uint32_t>* deviceLost  = nullptr;

    [[nodiscard]] constexpr bool Valid() const noexcept {
        return validation != nullptr && deviceLost != nullptr;
    }
};

// ============================================================================
// Vk::Instance — RAII owner of the Vulkan instance, its persistent debug
// messenger, and the validation/device-lost diagnostics.
//
// The C layer (RenderCore.c) is stateless; the counters its debug callbacks
// used to bump in C globals are routed into CALLER-OWNED storage:
//
//   * With a registered sink, increments go straight to the caller's atomics
//     (single source of truth -- nothing to fold at retirement, so totals
//     are exact across any number of sequential create/destroy cycles).
//   * Without one, each instance counts into its own members; those counts
//     are a live view and die with the instance.
//
// The instance descriptor carries a ZHLN_DebugForwarding pointing back here,
// so both the pNext messenger (instance create/destroy coverage) and the
// persistent messenger (runtime coverage) funnel error severities in.
//
// The engine is single-instance by design: volk's dispatch tables are
// process-global and cannot serve two live instances. Create() claims the
// slot with a compare-and-swap and refuses -- returning an invalid Instance
// -- while another instance is live, instead of letting a second one
// silently steal the slot.
//
// Reads (ValidationErrorCount/DeviceLostCount) must not race the destruction
// of the instance they observe. Callers bracketing engine lifetimes register
// a sink and read their own storage instead -- that is exactly what it is
// for.
// ============================================================================
class Instance {
  public:
    Instance() noexcept: _debugForwarding(std::unique_ptr<ZHLN_DebugForwarding>(new (std::nothrow) ZHLN_DebugForwarding {})) {
    }
    ~Instance() noexcept;

    Instance(const Instance&)                    = delete;
    auto operator=(const Instance&) -> Instance& = delete;

    Instance(Instance&& other) noexcept;
    auto operator=(Instance&& other) noexcept -> Instance&;

    // Creates the Vulkan instance (acquiring the loader through Volk) and,
    // when validation is enabled, the persistent debug messenger. Returns an
    // invalid Instance on failure (Valid() == false) -- including when
    // another instance is still live (single-instance engine; see above).
    [[nodiscard]] static auto
        Create(std::string_view appName, uint32_t appVersion, std::span<const std::string_view> extensions, ZHLN_ValidationMode validation) noexcept -> Instance;

    // Registers caller-owned diagnostics storage (both or neither; pass a
    // default-constructed sink to revert to per-instance counting). Engines
    // created afterwards increment these atomics directly, including
    // teardown-time events, so deltas across an engine's full lifecycle are
    // exact. The storage must outlive every engine created after
    // registration. Register before creating engines: the sink is resolved
    // once per Create(), so it is not synchronised against concurrent engine
    // creation.
    static void UseDiagnostics(DiagnosticsSink sink) noexcept;

    [[nodiscard]] auto Handle() const noexcept -> VkInstance {
        return _handle;
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _handle != VK_NULL_HANDLE;
    }

    // --- Live diagnostics (the active instance's view) ---------------------
    // Zero when no engine exists. These read the instance that is alive NOW;
    // observers needing values across an engine's death hold a registered
    // sink instead of polling these.

    [[nodiscard]] static auto ValidationErrorCount() noexcept -> uint32_t;
    [[nodiscard]] static auto DeviceLostCount() noexcept -> uint32_t;

    // Records a device-lost event observed by engine code (failed submits,
    // VK_ERROR_DEVICE_LOST returns). Bumps the active instance's counting
    // target; with no live instance the event is unobservable by design.
    static void NotifyDeviceLost() noexcept;

    // The single live instance, or null when none exists (engine is
    // single-instance by design).
    [[nodiscard]] static auto Active() noexcept -> Instance* {
        return _active.load(std::memory_order::acquire);
    }

  private:
    static void DebugHookTrampoline(void* userdata, VkDebugUtilsMessageSeverityFlagBitsEXT severity) noexcept;

    VkInstance               _handle    = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT _messenger = VK_NULL_HANDLE;
    // Stable heap storage: Vulkan stores this pointer as pUserData in both the
    // instance-create callback chain and the persistent debug messenger, so it
    // must remain valid across Instance moves.
    std::unique_ptr<ZHLN_DebugForwarding> _debugForwarding;

    // Counting targets: the registered sink when one was resolved at
    // Create() time, else this instance's own members.
    std::atomic<uint32_t>  _validationErrors {0};
    std::atomic<uint32_t>  _deviceLost {0};
    std::atomic<uint32_t>* _validationTarget = &_validationErrors;
    std::atomic<uint32_t>* _deviceLostTarget = &_deviceLost;

    static std::atomic<Instance*>       _active;
    static std::atomic<DiagnosticsSink> _registeredSink;
};

} // namespace ZHLN::Vk
