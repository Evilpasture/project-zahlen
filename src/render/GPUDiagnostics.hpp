// src/render/GPUDiagnostics.hpp
#pragma once

#include <concepts>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace ZHLN::Vk {

enum class GPUVendor : uint16_t { Unknown = 0, NVIDIA = 0x10DE, AMD = 0x1002, Intel = 0x8086, ARM = 0x13B5, Qualcomm = 0x5143 };

struct DiagnosticConfig {
    bool enableMarkers     = true; // Breadcrumbs per pass/draw
    bool enableShaderDebug = true; // Line-level fault tracking
    bool enableCrashDumps  = true; // Write .nv-gpudmp / log artifacts
};

// Compile-time concept verifying that all tracker types satisfy the interface
template <typename T>
concept GPUCrashTrackerBackend = requires(
    T                         t,
    VkPhysicalDevice          physical,
    VkDevice                  device,
    void**                    ppNext,
    std::vector<const char*>& exts,
    VkCommandBuffer           cmd,
    std::string_view          name,
    std::span<const uint32_t> spv
) {
    { t.PreDeviceCreate(physical, ppNext, exts) } -> std::same_as<void>;
    { t.PostDeviceCreate(device, physical) } -> std::same_as<void>;
    { t.WriteCheckpoint(cmd, name) } -> std::same_as<void>;
    { t.RegisterShader(spv, name) } -> std::same_as<void>;
    { t.OnDeviceLost() } -> std::same_as<void>;
    { t.Shutdown() } -> std::same_as<void>;
};

struct NvidiaAftermathTracker {
    explicit NvidiaAftermathTracker(DiagnosticConfig cfg = {}): config(cfg) {
    }

    void PreDeviceCreate(VkPhysicalDevice physical, void** ppNext, std::vector<const char*>& exts);
    void PostDeviceCreate(VkDevice device, VkPhysicalDevice physical);
    void WriteCheckpoint(VkCommandBuffer cmd, std::string_view name);
    void RegisterShader(std::span<const uint32_t> spirv, std::string_view entryPoint);
    void OnDeviceLost();
    void Shutdown();

    DiagnosticConfig config;
    VkDevice         device = VK_NULL_HANDLE;
};

struct AmdBreadcrumbTracker {
    explicit AmdBreadcrumbTracker(DiagnosticConfig cfg = {}): config(cfg) {
    }

    void PreDeviceCreate(VkPhysicalDevice physical, void** ppNext, std::vector<const char*>& exts);
    void PostDeviceCreate(VkDevice device, VkPhysicalDevice physical);
    void WriteCheckpoint(VkCommandBuffer cmd, std::string_view name);
    void RegisterShader(std::span<const uint32_t> spirv, std::string_view entryPoint);
    void OnDeviceLost();
    void Shutdown();

    DiagnosticConfig              config;
    VkDevice                      device               = VK_NULL_HANDLE;
    PFN_vkCmdWriteBufferMarkerAMD cmdWriteBufferMarker = nullptr;
};

// ----------------------------------------------------------------------------
// The Unified Variant Manager
// ----------------------------------------------------------------------------
class GPUDiagnostics {
  public:
    GPUDiagnostics() = default;
    ~GPUDiagnostics() noexcept {
        Shutdown();
    }

    void SelectBackend(GPUVendor vendor, DiagnosticConfig config = {}) {
        switch (vendor) {
            case GPUVendor::NVIDIA:
                _tracker.emplace<NvidiaAftermathTracker>(config);
                break;
            case GPUVendor::AMD:
                _tracker.emplace<AmdBreadcrumbTracker>(config);
                break;
            default:
                _tracker.emplace<NullTracker>();
                break;
        }
    }

    [[gnu::always_inline]]
    void PreDeviceCreate(VkPhysicalDevice physical, void** ppNextChain, std::vector<const char*>& exts) noexcept {
        std::visit([&](auto& backend) noexcept { backend.PreDeviceCreate(physical, ppNextChain, exts); }, _tracker);
    }

    [[gnu::always_inline]]
    void PostDeviceCreate(VkDevice device, VkPhysicalDevice physical) noexcept {
        std::visit([&](auto& backend) noexcept { backend.PostDeviceCreate(device, physical); }, _tracker);
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
    // Hidden private implementation detail
    struct NullTracker {
        void PreDeviceCreate(VkPhysicalDevice /*unused*/, void** /*unused*/, std::vector<const char*>& /*unused*/) noexcept {
        }
        void PostDeviceCreate(VkDevice /*unused*/, VkPhysicalDevice /*unused*/) noexcept {
        }
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

    using TrackerVariant = std::variant<NullTracker, NvidiaAftermathTracker, AmdBreadcrumbTracker>;

    // Automatically default-constructs into index 0 (NullTracker)
    TrackerVariant _tracker;
};

} // namespace ZHLN::Vk
