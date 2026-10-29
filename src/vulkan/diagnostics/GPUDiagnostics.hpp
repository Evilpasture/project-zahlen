// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../core/Context.hpp"
#include "GPUAddressTracker.hpp"
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace ZHLN::Vk {

struct DiagnosticConfig {
    bool        enableMarkers    = true;
    bool        enableCrashDumps = true;
    std::string crashDumpPath;
};

class GPUDiagnostics {
  public:
    GPUDiagnostics() noexcept = default;
    ~GPUDiagnostics() noexcept {
        Shutdown();
    }

    // Owns `_addressTracker` at a stable address for `VK_EXT_device_address_binding_report`
    // messenger callbacks; must not move once created.
    GPUDiagnostics(const GPUDiagnostics&)                    = delete;
    auto operator=(const GPUDiagnostics&) -> GPUDiagnostics& = delete;
    GPUDiagnostics(GPUDiagnostics&&)                         = delete;
    auto operator=(GPUDiagnostics&&) -> GPUDiagnostics&      = delete;

    void Create(Context& ctx, DiagnosticConfig config = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] auto AddressTracker() noexcept -> GPUAddressTracker& {
        return _addressTracker;
    }
    [[nodiscard]] auto AddressTracker() const noexcept -> const GPUAddressTracker& {
        return _addressTracker;
    }

    // --- HOT PATH: Zero virtual/variant overhead ---
    [[gnu::always_inline]]
    void WriteCheckpoint(VkCommandBuffer cmd, std::string_view name) const noexcept {
        if (!_markersActive || cmd == VK_NULL_HANDLE || name.empty()) [[likely]] {
            return;
        }
        WriteCheckpointSlow(cmd, name);
    }

    // --- COLD PATH: Invoked only on VK_ERROR_DEVICE_LOST ---
    void OnDeviceLost() const noexcept;

    [[nodiscard]] bool IsActive() const noexcept {
        return _device != VK_NULL_HANDLE;
    }
    [[nodiscard]] const DiagnosticConfig& Config() const noexcept {
        return _config;
    }

  private:
    static auto VKAPI_CALL AddressBindingCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
        VkDebugUtilsMessageTypeFlagsEXT             type,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
        void*                                       userData
    ) noexcept -> VkBool32;

    void WriteCheckpointSlow(VkCommandBuffer cmd, std::string_view name) const noexcept;

    Context*                 _ctx                     = nullptr;
    VkInstance               _instance                = VK_NULL_HANDLE;
    VkDevice                 _device                  = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT _addressBindingMessenger = VK_NULL_HANDLE;
    GPUAddressTracker        _addressTracker {};
    DiagnosticConfig         _config {};
    bool                     _markersActive = false;
};

} // namespace ZHLN::Vk
