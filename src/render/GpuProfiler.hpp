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

	~GpuProfiler() noexcept {
		if (_device != VK_NULL_HANDLE) {
			for (uint32_t i = 0; i < 2; ++i) {
				if (_pools[i] != VK_NULL_HANDLE) {
					vkDestroyQueryPool(_device, _pools[i], nullptr);
				}
			}
		}
	}

	// Move-only semantics matching ZHLN design
	GpuProfiler(const GpuProfiler&) = delete;
	auto operator=(const GpuProfiler&) -> GpuProfiler& = delete;

	GpuProfiler(GpuProfiler&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _pools(std::exchange(other._pools, {VK_NULL_HANDLE, VK_NULL_HANDLE})) {}

	auto operator=(GpuProfiler&& other) noexcept -> GpuProfiler& {
		if (this != &other) {
			if (_device != VK_NULL_HANDLE) {
				for (uint32_t i = 0; i < 2; ++i) {
					vkDestroyQueryPool(_device, _pools[i], nullptr);
				}
			}
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_pools = std::exchange(other._pools, {VK_NULL_HANDLE, VK_NULL_HANDLE});
		}
		return *this;
	}

	void Init(VkDevice device) noexcept {
		_device = device;
		VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
									  .pNext = nullptr,
									  .flags = 0,
									  .queryType = VK_QUERY_TYPE_TIMESTAMP,
									  .queryCount = kQueryCount,
									  .pipelineStatistics = 0};

		for (uint32_t i = 0; i < 2; ++i) {
			vkCreateQueryPool(device, &info, nullptr, &_pools[i]);
			// Perform initial reset to clear garbage memory
			vkResetQueryPool(device, _pools[i], 0, kQueryCount);
		}
	}

	/**
	 * @brief Resets the query pool on the CPU before recording.
	 */
	void Reset(uint32_t frameIndex) noexcept {
		vkResetQueryPool(_device, _pools[frameIndex % 2], 0, kQueryCount);
	}

	// --- Compile-Time Resolved Writes ---

	template <GpuStageTag Stage>
	void WriteStart(VkCommandBuffer cmd, uint32_t frameIndex) noexcept {
		static_assert(ContainsType<Stage, Stages...>,
					  "Stage tag not registered in this GpuProfiler!");
		constexpr uint32_t queryIdx = TypeIndex<Stage, Stages...>::value * 2;

		vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, _pools[frameIndex % 2],
							 queryIdx);
	}

	template <GpuStageTag Stage> void WriteEnd(VkCommandBuffer cmd, uint32_t frameIndex) noexcept {
		static_assert(ContainsType<Stage, Stages...>,
					  "Stage tag not registered in this GpuProfiler!");
		constexpr uint32_t queryIdx = (TypeIndex<Stage, Stages...>::value * 2) + 1;

		vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, _pools[frameIndex % 2],
							 queryIdx);
	}

	// --- Results Extraction ---

	/**
	 * @brief Pulls completed results from the GPU and processes them via a fold expression.
	 *
	 * @tparam Func Callback signature: void(std::string_view name, float durationMS)
	 */
	template <typename Func>
	void RetrieveResults(uint32_t frameIndex, float timestampPeriod, Func&& callback) noexcept {
		VkQueryPool pool = _pools[frameIndex % 2];
		std::array<uint64_t, kQueryCount> rawResults{};

		VkResult res = vkGetQueryPoolResults(
			_device, pool, 0, kQueryCount, rawResults.size() * sizeof(uint64_t), rawResults.data(),
			sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

		if (res != VK_SUCCESS) [[unlikely]] {
			return; // GPU still evaluating or out of date
		}

		// Compile-time Fold Expansion: Walks through every registered stage sequentially
		(
			[&]<typename Stage>() {
				constexpr uint32_t startIdx = TypeIndex<Stage, Stages...>::value * 2;
				constexpr uint32_t endIdx = startIdx + 1;

				uint64_t startTicks = rawResults[startIdx];
				uint64_t endTicks = rawResults[endIdx];

				float durationMS = 0.0f;
				if (endTicks >= startTicks) {
					durationMS =
						static_cast<float>(endTicks - startTicks) * timestampPeriod / 1000000.0f;
				}
				callback(Stage::name, durationMS);
			}.template operator()<Stages>(),
			...);
	}

  private:
	VkDevice _device = VK_NULL_HANDLE;
	std::array<VkQueryPool, 2> _pools = {VK_NULL_HANDLE, VK_NULL_HANDLE};
};

// ============================================================================
// RAII Compile-Time Scope Guard
// ============================================================================

template <GpuStageTag Stage, typename ProfilerT> class ScopedGpuProfile {
  public:
	ScopedGpuProfile(VkCommandBuffer cmd, uint32_t frameIndex, ProfilerT& profiler) noexcept
		: _cmd(cmd), _frameIndex(frameIndex), _profiler(profiler) {
		_profiler.template WriteStart<Stage>(_cmd, _frameIndex);
	}

	~ScopedGpuProfile() noexcept { _profiler.template WriteEnd<Stage>(_cmd, _frameIndex); }

	ScopedGpuProfile(const ScopedGpuProfile&) = delete;
	auto operator=(const ScopedGpuProfile&) -> ScopedGpuProfile& = delete;

  private:
	VkCommandBuffer _cmd;
	uint32_t _frameIndex;
	ProfilerT& _profiler;
};

} // namespace ZHLN::Profiler
