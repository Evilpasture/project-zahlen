// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/GpuProfiler.hpp

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Reflection.hpp>

namespace ZHLN::Profiler {

// ============================================================================
// Double-Buffered Reflection-Driven GPU Profiler
// ============================================================================

template <typename EnumT>
    requires std::is_enum_v<EnumT>
class GpuProfiler {
  public:
    static constexpr uint32_t kStageCount = static_cast<uint32_t>(Reflect::EnumCount<EnumT>());
    static constexpr uint32_t kQueryCount = kStageCount * 2; // Start & End for each stage

    GpuProfiler() noexcept = default;
    ~GpuProfiler() noexcept;

    // Move-only semantics matching ZHLN design
    GpuProfiler(const GpuProfiler&)                    = delete;
    auto operator=(const GpuProfiler&) -> GpuProfiler& = delete;

    GpuProfiler(GpuProfiler&& other) noexcept;
    auto operator=(GpuProfiler&& other) noexcept -> GpuProfiler&;

    void Init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex) noexcept;

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

  private:
    VkDevice                        _device        = VK_NULL_HANDLE;
    std::array<VkQueryPool, 2>      _pools         = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    mutable std::array<uint32_t, 2> _recordedMasks = {0, 0};
    bool                            _enabled       = false;
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
