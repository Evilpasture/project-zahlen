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

struct GpuAllocationSymbol {
    VkDeviceAddress                  baseAddress      = 0;
    VkDeviceSize                     size             = 0;
    uint64_t                         objectHandle     = 0;
    VkObjectType                     objectType       = VK_OBJECT_TYPE_UNKNOWN;
    VkDeviceAddressBindingFlagsEXT   flags            = 0;
    bool                             isDriverInternal = false;
    std::string                      name;
};

class GPUAddressTracker {
  public:
    GPUAddressTracker() noexcept = default;

    void SetEnabled(bool enabled) noexcept;
    [[nodiscard]] auto Enabled() const noexcept -> bool;

    void OnBindingEvent(
        const VkDeviceAddressBindingCallbackDataEXT& binding,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData
    ) noexcept;

    void SetObjectName(VkObjectType type, uint64_t handle, std::string_view name) noexcept;

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
