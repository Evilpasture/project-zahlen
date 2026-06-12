// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


// src/render/GpuProfiler.hpp

#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vulkan/vulkan.h>

namespace ZHLN::Profiler {

// ============================================================================
// Compile-Time Type Helpers (TMP)
// ============================================================================

template <typename T, typename... List>
concept ContainsType = (std::same_as<T, List> || ...);

template <typename T, typename... Us> struct TypeIndex;

template <typename T, typename... Us> struct TypeIndex<T, T, Us...> {
	static constexpr size_t value = 0;
};

template <typename T, typename U, typename... Us> struct TypeIndex<T, U, Us...> {
	static constexpr size_t value = 1 + TypeIndex<T, Us...>::value;
};

// Concept for Stage Tags
template <typename T>
concept GpuStageTag = requires {
	{ T::name } -> std::convertible_to<std::string_view>;
};

// ============================================================================
// Double-Buffered TMP GPU Profiler
// ============================================================================

template <GpuStageTag... Stages> class GpuProfiler {
  public:
	static constexpr uint32_t kStageCount = sizeof...(Stages);
	static constexpr uint32_t kQueryCount = kStageCount * 2; // Start & End for each stage

	GpuProfiler() noexcept = default;
	~GpuProfiler() noexcept;

	// Move-only semantics matching ZHLN design
	GpuProfiler(const GpuProfiler&) = delete;
	auto operator=(const GpuProfiler&) -> GpuProfiler& = delete;

	GpuProfiler(GpuProfiler&& other) noexcept;
	auto operator=(GpuProfiler&& other) noexcept -> GpuProfiler&;

	void Init(VkDevice device) noexcept;

	/**
	 * @brief Resets the query pool on the CPU before recording.
	 */
	void Reset(uint32_t frameIndex) noexcept;

	// --- Compile-Time Resolved Writes ---

	template <GpuStageTag Stage> void WriteStart(VkCommandBuffer cmd, uint32_t frameIndex) noexcept;

	template <GpuStageTag Stage> void WriteEnd(VkCommandBuffer cmd, uint32_t frameIndex) noexcept;

	// --- Results Extraction ---

	/**
	 * @brief Pulls completed results from the GPU and processes them via a fold expression.
	 *
	 * @tparam Func Callback signature: void(std::string_view name, float durationMS)
	 */
	template <typename Func>
	void RetrieveResults(uint32_t frameIndex, float timestampPeriod, Func&& callback) noexcept;

  private:
	VkDevice _device = VK_NULL_HANDLE;
	std::array<VkQueryPool, 2> _pools = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	std::array<uint32_t, 2> _recordedMasks = {0, 0}; // Double-buffered stage record mask [2]
};

// ============================================================================
// RAII Compile-Time Scope Guard
// ============================================================================

template <GpuStageTag Stage, typename ProfilerT> class ScopedGpuProfile {
  public:
	ScopedGpuProfile(VkCommandBuffer cmd, uint32_t frameIndex, ProfilerT& profiler) noexcept;
	~ScopedGpuProfile() noexcept;

	ScopedGpuProfile(const ScopedGpuProfile&) = delete;
	auto operator=(const ScopedGpuProfile&) -> ScopedGpuProfile& = delete;

  private:
	VkCommandBuffer _cmd;
	uint32_t _frameIndex;
	ProfilerT& _profiler;
};

} // namespace ZHLN::Profiler

#include "GpuProfiler.inl"
