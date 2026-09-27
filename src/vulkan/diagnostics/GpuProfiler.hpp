// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Reflection/Enums.hpp>

namespace ZHLN::Profiler {

enum class GpuProfilerError : uint8_t {
    QueryPoolCreationFailed      ZHLN_ANNOTATION(ZHLN::Description<"Timestamp query pool creation failed"> {})           = 1,
    StatsQueryPoolCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Pipeline statistics query pool creation failed"> {}) = 2,
};


struct PipelineStats {
    uint64_t iaVertices           = 0;
    uint64_t iaPrimitives         = 0;
    uint64_t vsInvocations        = 0;
    uint64_t clipperInvocations   = 0;
    uint64_t clipperPrimitivesOut = 0;
    uint64_t gsInvocations        = 0;
    uint64_t gsPrimitives         = 0;
    uint64_t fsInvocations        = 0;
    uint64_t csInvocations        = 0;
    uint64_t taskInvocations      = 0;
    uint64_t meshInvocations      = 0;

    [[nodiscard]] auto ClippedFraction() const noexcept -> double {
        return (clipperInvocations > 0) ? 1.0 - static_cast<double>(clipperPrimitivesOut) / static_cast<double>(clipperInvocations) : 0.0;
    }
};


template <typename EnumT>
    requires std::is_enum_v<EnumT>
class GpuProfiler {
  public:
    using StageType = EnumT;

    static constexpr uint32_t kStageCount = static_cast<uint32_t>(Reflect::EnumCount<EnumT>());
    static constexpr uint32_t kQueryCount = kStageCount * 2;
    static_assert(kStageCount <= 64, "GpuProfiler currently supports at most 64 reflected stages.");

    GpuProfiler() noexcept = default;
    ~GpuProfiler() noexcept;

    GpuProfiler(const GpuProfiler&)                    = delete;
    auto operator=(const GpuProfiler&) -> GpuProfiler& = delete;

    GpuProfiler(GpuProfiler&& other) noexcept;
    auto operator=(GpuProfiler&& other) noexcept -> GpuProfiler&;

    [[nodiscard]] auto
        Init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, bool meshPipelineStats) noexcept -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto Enabled() const noexcept -> bool {
        return _enabled;
    }

    void Reset(uint32_t frameIndex) noexcept;


    void WriteStart(VkCommandBuffer cmd, uint32_t frameIndex, EnumT stage) const noexcept;
    void WriteEnd(VkCommandBuffer cmd, uint32_t frameIndex, EnumT stage) const noexcept;

    template <typename Func>
    void RetrieveResults(uint32_t frameIndex, float timestampPeriod, Func&& callback) noexcept;


    [[nodiscard]] auto PipelineStatsAvailable() const noexcept -> bool {
        return _statsSupported;
    }

    void SetPipelineStatsEnabled(bool enabled) noexcept {
        _statsEnabled = enabled && _statsSupported;
    }

    [[nodiscard]] auto PipelineStatsEnabled() const noexcept -> bool {
        return _statsEnabled;
    }

    template <typename Func>
    void RetrievePipelineStats(uint32_t frameIndex, Func&& callback) noexcept;

  private:
    void Teardown() noexcept;

    VkDevice                        _device        = VK_NULL_HANDLE;
    std::array<VkQueryPool, 2>      _pools         = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    mutable std::array<uint64_t, 2> _recordedMasks = {0, 0};
    bool                            _enabled       = false;

    std::array<VkQueryPool, 2>      _statsPools      = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    mutable std::array<uint64_t, 2> _statsBeginMasks = {0, 0};
    mutable std::array<uint64_t, 2> _statsEndMasks   = {0, 0};
    VkQueryPipelineStatisticFlags   _statsBits       = 0;
    bool                            _statsSupported  = false;
    bool                            _statsEnabled    = false;
};


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

template <typename EnumT>
ScopedGpuProfile(VkCommandBuffer, uint32_t, const GpuProfiler<EnumT>&, EnumT) -> ScopedGpuProfile<EnumT>;

}

#include "GpuProfiler.inl"
