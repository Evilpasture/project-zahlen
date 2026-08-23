// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace ZHLN::Vk {

enum class GPUVendor : uint16_t { Unknown = 0, NVIDIA = 0x10DE, AMD = 0x1002, Intel = 0x8086, ARM = 0x13B5, Qualcomm = 0x5143 };

struct DiagnosticConfig {
    bool enableMarkers     = true;
    bool enableShaderDebug = true;
    bool enableCrashDumps  = true;
};

/** Renderer-internal interface implemented by an already-created backend. */
template <typename T>
concept GPUCrashTrackerBackend = requires(
    T                         t,
    VkCommandBuffer           cmd,
    std::string_view          name,
    std::span<const uint32_t> spv
) {
    { t.WriteCheckpoint(cmd, name) } -> std::same_as<void>;
    { t.RegisterShader(spv, name) } -> std::same_as<void>;
    { t.OnDeviceLost() } -> std::same_as<void>;
    { t.Shutdown() } -> std::same_as<void>;
};

struct GPUCrashTrackerCallbacks {
    std::function<void(VkCommandBuffer, std::string_view)>           writeCheckpoint;
    std::function<void(std::span<const uint32_t>, std::string_view)> registerShader;
    std::function<void()>                                            onDeviceLost;
    std::function<void()>                                            shutdown;

    [[nodiscard]] bool Empty() const noexcept {
        return !writeCheckpoint && !registerShader && !onDeviceLost && !shutdown;
    }
};

template <GPUCrashTrackerBackend Backend>
[[nodiscard]] GPUCrashTrackerCallbacks MakeGPUCrashTrackerCallbacks(std::shared_ptr<Backend> backend) {
    if (!backend) {
        return {};
    }

    return {
        .writeCheckpoint = [backend](VkCommandBuffer cmd, std::string_view name) { backend->WriteCheckpoint(cmd, name); },
        .registerShader  = [backend](std::span<const uint32_t> spirv, std::string_view name) { backend->RegisterShader(spirv, name); },
        .onDeviceLost    = [backend] { backend->OnDeviceLost(); },
        .shutdown        = [backend] { backend->Shutdown(); },
    };
}

template <typename Backend>
    requires GPUCrashTrackerBackend<std::remove_cvref_t<Backend>>
[[nodiscard]] GPUCrashTrackerCallbacks MakeGPUCrashTrackerCallbacks(Backend&& backend) {
    using BackendT = std::remove_cvref_t<Backend>;
    return MakeGPUCrashTrackerCallbacks(std::make_shared<BackendT>(std::forward<Backend>(backend)));
}

/**
 * Build-time extension point. A configured source creates and initializes its
 * native backend here; Zahlen supplies a null implementation by default.
 */
[[nodiscard]] GPUCrashTrackerCallbacks CreateConfiguredGPUCrashTracker(
    GPUVendor vendor,
    VkDevice device,
    VkPhysicalDevice physical,
    DiagnosticConfig config
);

struct CallbackCrashTracker {
    explicit CallbackCrashTracker(GPUCrashTrackerCallbacks hooks): callbacks(std::move(hooks)) {
    }

    void WriteCheckpoint(VkCommandBuffer cmd, std::string_view name) {
        if (callbacks.writeCheckpoint) {
            callbacks.writeCheckpoint(cmd, name);
        }
    }
    void RegisterShader(std::span<const uint32_t> spirv, std::string_view entryPoint) {
        if (callbacks.registerShader) {
            callbacks.registerShader(spirv, entryPoint);
        }
    }
    void OnDeviceLost() {
        if (callbacks.onDeviceLost) {
            callbacks.onDeviceLost();
        }
    }
    void Shutdown() {
        if (callbacks.shutdown) {
            callbacks.shutdown();
        }
    }

    GPUCrashTrackerCallbacks callbacks;
};
static_assert(GPUCrashTrackerBackend<CallbackCrashTracker>);

struct DebugUtilsTracker {
    DebugUtilsTracker(VkDevice device, DiagnosticConfig config);

    void WriteCheckpoint(VkCommandBuffer cmd, std::string_view name);
    void RegisterShader(std::span<const uint32_t> spirv, std::string_view entryPoint);
    void OnDeviceLost();
    void Shutdown();

    DiagnosticConfig                  config;
    VkDevice                          device              = VK_NULL_HANDLE;
    PFN_vkCmdInsertDebugUtilsLabelEXT cmdInsertDebugLabel = nullptr;
};

class GPUDiagnostics {
  public:
    GPUDiagnostics() = default;
    ~GPUDiagnostics() noexcept {
        Shutdown();
    }

    GPUDiagnostics(const GPUDiagnostics&)                    = delete;
    auto operator=(const GPUDiagnostics&) -> GPUDiagnostics& = delete;
    GPUDiagnostics(GPUDiagnostics&&)                         = delete;
    auto operator=(GPUDiagnostics&&) -> GPUDiagnostics&      = delete;

    void Create(GPUVendor vendor, VkDevice device, VkPhysicalDevice physical, DiagnosticConfig config = {}) {
        Shutdown();

        auto configured = CreateConfiguredGPUCrashTracker(vendor, device, physical, config);
        if (!configured.Empty()) {
            _tracker.emplace<CallbackCrashTracker>(std::move(configured));
        } else {
            _tracker.emplace<DebugUtilsTracker>(device, config);
        }
    }

    [[gnu::always_inline]]
    void WriteCheckpoint(VkCommandBuffer cmd, std::string_view name) noexcept {
        std::visit([&](auto& backend) noexcept { backend.WriteCheckpoint(cmd, name); }, _tracker);
    }

    [[gnu::always_inline]]
    void RegisterShader(std::span<const uint32_t> spirv, std::string_view entryPoint) noexcept {
        std::visit([&](auto& backend) noexcept { backend.RegisterShader(spirv, entryPoint); }, _tracker);
    }

    [[gnu::always_inline]]
    void OnDeviceLost() noexcept {
        std::visit([](auto& backend) noexcept { backend.OnDeviceLost(); }, _tracker);
    }

    [[gnu::always_inline]]
    void Shutdown() noexcept {
        std::visit([](auto& backend) noexcept { backend.Shutdown(); }, _tracker);
        _tracker.emplace<NullTracker>();
    }

    [[nodiscard]] bool IsActive() const noexcept {
        return !std::holds_alternative<NullTracker>(_tracker);
    }

  private:
    struct NullTracker {
        void WriteCheckpoint(VkCommandBuffer /*unused*/, std::string_view /*unused*/) noexcept {
        }
        void RegisterShader(std::span<const uint32_t> /*unused*/, std::string_view /*unused*/) noexcept {
        }
        void OnDeviceLost() noexcept {
        }
        void Shutdown() noexcept {
        }
    };
    static_assert(GPUCrashTrackerBackend<NullTracker>);

    using TrackerVariant = std::variant<NullTracker, CallbackCrashTracker, DebugUtilsTracker>;
    TrackerVariant _tracker;
};

} // namespace ZHLN::Vk
