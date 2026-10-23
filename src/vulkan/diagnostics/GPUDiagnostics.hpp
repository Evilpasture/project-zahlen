// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

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

    GPUDiagnostics(const GPUDiagnostics&)                        = delete;
    auto operator=(const GPUDiagnostics&) -> GPUDiagnostics&     = delete;
    GPUDiagnostics(GPUDiagnostics&&) noexcept                    = default;
    auto operator=(GPUDiagnostics&&) noexcept -> GPUDiagnostics& = default;

    void Create(VkDevice device, GPUAddressTracker* addressTracker, DiagnosticConfig config = {}) noexcept;
    void Shutdown() noexcept;

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
    void WriteCheckpointSlow(VkCommandBuffer cmd, std::string_view name) const noexcept;
    void DumpDeviceFault() const noexcept;

    VkDevice           _device         = VK_NULL_HANDLE;
    GPUAddressTracker* _addressTracker = nullptr;
    DiagnosticConfig   _config {};
    bool               _markersActive = false;
};

} // namespace ZHLN::Vk
