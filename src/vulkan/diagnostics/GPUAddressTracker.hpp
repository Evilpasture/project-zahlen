// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Vk {

// One live device-address binding reported by VK_EXT_device_address_binding_report.
// The associated object comes from VkDebugUtilsMessengerCallbackDataEXT::pObjects;
// it is deliberately kept as diagnostic data only and is never passed back to Vulkan.
struct GpuAllocationSymbol {
    VkDeviceAddress                  baseAddress      = 0;
    VkDeviceSize                     size             = 0;
    uint64_t                         objectHandle     = 0;
    VkObjectType                     objectType       = VK_OBJECT_TYPE_UNKNOWN;
    VkDeviceAddressBindingFlagsEXT   flags            = 0;
    bool                             isDriverInternal = false;
    std::string                      name;
};

// Thread-safe, fault-time address resolver. Address-binding callbacks are
// synchronous with Vulkan calls and may arrive concurrently from multiple
// application threads, so the tracker does not use the engine's fiber mutex.
// Owned per Instance; there is no process-wide singleton.
class GPUAddressTracker {
  public:
    GPUAddressTracker() noexcept = default;

    // Starts a fresh device lifetime when enabled; disabling clears all ranges
    // after vkDestroyDevice has delivered its final unbind events.
    void SetEnabled(bool enabled) noexcept;
    [[nodiscard]] auto Enabled() const noexcept -> bool;

    void OnBindingEvent(
        const VkDeviceAddressBindingCallbackDataEXT& binding,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData
    ) noexcept;

    // Keep debug-utils object names in sync with currently live address ranges.
    // Names set after a bind update already-recorded symbols immediately; names
    // present on callback pObjects are copied when the bind event arrives.
    void SetObjectName(VkObjectType type, uint64_t handle, std::string_view name) noexcept;

    // An address can be shared by aliased resources, so Resolve returns every
    // matching live object rather than choosing an arbitrary overlapping range.
    [[nodiscard]] auto Resolve(VkDeviceAddress address) const noexcept -> std::vector<GpuAllocationSymbol>;
    [[nodiscard]] auto LiveBindingCount() const noexcept -> std::size_t;

  private:
    void ProcessBindingObjectLocked(
        const VkDeviceAddressBindingCallbackDataEXT& binding,
        VkObjectType type,
        uint64_t handle,
        const char* name
    ) noexcept;
    void BindLocked(
        const VkDeviceAddressBindingCallbackDataEXT& binding,
        VkObjectType type,
        uint64_t handle,
        const char* name
    ) noexcept;
    void UnbindLocked(
        const VkDeviceAddressBindingCallbackDataEXT& binding,
        VkObjectType type,
        uint64_t handle,
        bool objectKnown
    ) noexcept;

    mutable std::mutex               _mutex;
    std::vector<GpuAllocationSymbol> _entries;
    std::atomic<bool>                _enabled {false};
};

} // namespace ZHLN::Vk
