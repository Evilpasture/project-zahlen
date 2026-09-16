// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/diagnostics/GpuProfiler.hpp

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Reflection.hpp>

namespace ZHLN::Profiler {

// Query-pool bring-up failure. Absent timestamp *support* is not one: that
// leaves the profiler disabled and Init() still succeeds.
enum class GpuProfilerError : uint8_t {
    QueryPoolCreationFailed      ZHLN_ANNOTATION(ZHLN::Description<"Timestamp query pool creation failed"> {})           = 1,
    StatsQueryPoolCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Pipeline statistics query pool creation failed"> {}) = 2,
};

// ============================================================================
// Pipeline Statistics Counters
// ============================================================================

// One scope's VK_QUERY_TYPE_PIPELINE_STATISTICS results. The nine core
// counters are always captured; the mesh/task counters additionally need the
// meshShaderQueries feature enabled at device creation and stay 0 without it.
//
// The ratios this exists to measure:
//   * Clipping: 1 - clipperPrimitivesOut / clipperInvocations is the fraction
//     of rasterized primitives the clipper discards.
//   * Meshlet culling: meshInvocations is the count of mesh workgroups the
//     GPU actually executed after task-level culling. Compare it against the
//     number of meshlets the scene issued (scene knowledge lives with the
//     caller, not here) to get the cull rate -- a task workgroup can spawn
//     many mesh workgroups, so no counter-only ratio expresses it.
struct PipelineStats {
    uint64_t iaVertices           = 0;
    uint64_t iaPrimitives         = 0;
    uint64_t vsInvocations        = 0;
    uint64_t clipperInvocations   = 0; // primitives fed to the clipper
    uint64_t clipperPrimitivesOut = 0; // primitives that survived clipping
    uint64_t gsInvocations        = 0;
    uint64_t gsPrimitives         = 0;
    uint64_t fsInvocations        = 0;
    uint64_t csInvocations        = 0;
    uint64_t taskInvocations      = 0; // task workgroups launched (VK_EXT_mesh_shader)
    uint64_t meshInvocations      = 0; // mesh workgroups executed post-culling

    /// Fraction of clipper input primitives discarded (0..1); 0 when nothing
    /// reached the clipper.
    [[nodiscard]] auto ClippedFraction() const noexcept -> double {
        return (clipperInvocations > 0) ? 1.0 - static_cast<double>(clipperPrimitivesOut) / static_cast<double>(clipperInvocations) : 0.0;
    }
};

// ============================================================================
// Double-Buffered Reflection-Driven GPU Profiler
// ============================================================================

template <typename EnumT>
    requires std::is_enum_v<EnumT>
class GpuProfiler {
  public:
    using StageType = EnumT;

    static constexpr uint32_t kStageCount = static_cast<uint32_t>(Reflect::EnumCount<EnumT>());
    static constexpr uint32_t kQueryCount = kStageCount * 2; // Start & End for each stage
    static_assert(kStageCount <= 64, "GpuProfiler currently supports at most 64 reflected stages.");

    GpuProfiler() noexcept = default;
    ~GpuProfiler() noexcept;

    // Move-only semantics matching ZHLN design
    GpuProfiler(const GpuProfiler&)                    = delete;
    auto operator=(const GpuProfiler&) -> GpuProfiler& = delete;

    GpuProfiler(GpuProfiler&& other) noexcept;
    auto operator=(GpuProfiler&& other) noexcept -> GpuProfiler&;

    /**
     * @brief Brings up the timestamp query pools and the opt-in pipeline
     * statistics pool.
     *
     * A device or queue family without timestamp support is NOT an error: the
     * profiler is left disabled, every accessor becomes a no-op, and this
     * returns success. Query Enabled() to tell that case apart. Only a pool the
     * driver refused to create is reported.
     *
     * @param meshPipelineStats Whether the task/mesh statistic bits may be
     * queried: they are legal only when the meshShaderQueries feature was
     * ENABLED on the device (VUID-VkQueryPoolCreateInfo-meshShaderQueries-07069),
     * so pass the device-creation state rather than probing the physical device.
     */
    [[nodiscard]] auto
        Init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, bool meshPipelineStats) noexcept -> std::expected<void, ErrorCode>;

    /// Whether timestamp queries are live. False after a successful Init means
    /// the hardware or the queue family does not offer them.
    [[nodiscard]] auto Enabled() const noexcept -> bool {
        return _enabled;
    }

    /**
     * @brief Resets the query pool on the CPU before recording.
     */
    void Reset(uint32_t frameIndex) noexcept;

    // --- Writes ---

    void WriteStart(VkCommandBuffer cmd, uint32_t frameIndex, EnumT stage) const noexcept;
    void WriteEnd(VkCommandBuffer cmd, uint32_t frameIndex, EnumT stage) const noexcept;

    // --- Results Extraction ---
    template <typename Func>
    void RetrieveResults(uint32_t frameIndex, float timestampPeriod, Func&& callback) noexcept;

    // --- Pipeline Statistics ---
    //
    // Opt-in counter capture around the same scopes the timestamps measure
    // (Begin/End query per stage). Statistics queries make drivers serialize
    // counter bookkeeping, so they stay OFF until a consumer asks -- this is
    // a measurement tool, not always-on telemetry. Requires the
    // pipelineStatisticsQuery device feature; absent support leaves
    // PipelineStatsAvailable() false and every call a no-op.

    /// Whether the device supports statistics queries (Init built the pools).
    [[nodiscard]] auto PipelineStatsAvailable() const noexcept -> bool {
        return _statsSupported;
    }

    /// Toggles capture for subsequently recorded scopes. Safe to flip at any
    /// point: an already-begun query is always closed by its matching
    /// WriteEnd, and scopes recorded before enabling simply have no counters.
    void SetPipelineStatsEnabled(bool enabled) noexcept {
        _statsEnabled = enabled && _statsSupported;
    }

    [[nodiscard]] auto PipelineStatsEnabled() const noexcept -> bool {
        return _statsEnabled;
    }

    /// Invokes callback(std::string_view stageName, const PipelineStats&)
    /// for every scope whose Begin/End pair completed in that frame.
    template <typename Func>
    void RetrievePipelineStats(uint32_t frameIndex, Func&& callback) noexcept;

  private:
    /// Destroys both query pools and forgets the device. Idempotent, so a
    /// failed Init and the destructor can both call it.
    void Teardown() noexcept;

    VkDevice                        _device        = VK_NULL_HANDLE;
    std::array<VkQueryPool, 2>      _pools         = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    mutable std::array<uint64_t, 2> _recordedMasks = {0, 0};
    bool                            _enabled       = false;

    // Pipeline statistics: one query per stage (a statistics query spans its
    // Begin/End pair, so no start/end doubling like the timestamp pools),
    // double-buffered like everything else here.
    std::array<VkQueryPool, 2>      _statsPools      = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    mutable std::array<uint64_t, 2> _statsBeginMasks = {0, 0};
    mutable std::array<uint64_t, 2> _statsEndMasks   = {0, 0};
    VkQueryPipelineStatisticFlags   _statsBits       = 0; // statistic bits the pools actually query
    bool                            _statsSupported  = false;
    bool                            _statsEnabled    = false;
};

// ============================================================================
// RAII Compile-Time Scope Guard
// ============================================================================

template <typename EnumT>
class ScopedGpuProfile {
  public:
    ScopedGpuProfile(VkCommandBuffer cmd, uint32_t frameIndex, const GpuProfiler<EnumT>& profiler, EnumT stage) noexcept;
    ~ScopedGpuProfile() noexcept;

    ScopedGpuProfile(const ScopedGpuProfile&)                    = delete;
    auto operator=(const ScopedGpuProfile&) -> ScopedGpuProfile& = delete;

  private:
    VkCommandBuffer           _cmd;
    uint32_t                  _frameIndex;
    const GpuProfiler<EnumT>& _profiler;
    EnumT                     _stage;
};

// CTAD Deduction Guide
template <typename EnumT>
ScopedGpuProfile(VkCommandBuffer, uint32_t, const GpuProfiler<EnumT>&, EnumT) -> ScopedGpuProfile<EnumT>;

} // namespace ZHLN::Profiler

#include "GpuProfiler.inl"
