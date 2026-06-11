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
		  _pools(std::exchange(other._pools, {VK_NULL_HANDLE, VK_NULL_HANDLE})),
		  _recordedMasks(std::exchange(other._recordedMasks, {0, 0})) {}

	auto operator=(GpuProfiler&& other) noexcept -> GpuProfiler& {
		if (this != &other) {
			if (_device != VK_NULL_HANDLE) {
				for (uint32_t i = 0; i < 2; ++i) {
					vkDestroyQueryPool(_device, _pools[i], nullptr);
				}
			}
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_pools = std::exchange(other._pools, {VK_NULL_HANDLE, VK_NULL_HANDLE});
			_recordedMasks = std::exchange(other._recordedMasks, {0, 0});
		}
		return *this;
	}

	void Init(VkDevice device) noexcept {
		_device = device;
		_recordedMasks = {0, 0};
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
		uint32_t slot = frameIndex % 2;
		vkResetQueryPool(_device, _pools[slot], 0, kQueryCount);
		_recordedMasks[slot] = 0; // Reset recorded stages for this frame slot [2]
	}

	// --- Compile-Time Resolved Writes ---

	template <GpuStageTag Stage>
	void WriteStart(VkCommandBuffer cmd, uint32_t frameIndex) noexcept {
		static_assert(ContainsType<Stage, Stages...>,
					  "Stage tag not registered in this GpuProfiler!");
		constexpr uint32_t stageIdx = TypeIndex<Stage, Stages...>::value;
		constexpr uint32_t queryIdx = stageIdx * 2;

		uint32_t slot = frameIndex % 2;
		_recordedMasks[slot] |= (1u << stageIdx); // Flag stage as recorded [2]

		vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, _pools[slot], queryIdx);
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
		uint32_t slot = frameIndex % 2;
		uint32_t mask = _recordedMasks[slot];
		if (mask == 0) {
			return; // No recorded stages, avoid querying entirely [2]
		}

		VkQueryPool pool = _pools[slot];

		// Compile-time Fold Expansion: Query each stage individually *only if it was recorded* [2]
		(
			[&]<typename Stage>() {
				constexpr uint32_t stageIdx = TypeIndex<Stage, Stages...>::value;
				if (mask & (1u << stageIdx)) {
					constexpr uint32_t startIdx = stageIdx * 2;
					std::array<uint64_t, 2> stageResults{};

					// Retrieve only the 2 timestamps for this specific stage [2]
					VkResult res = vkGetQueryPoolResults(
						_device, pool, startIdx, 2, stageResults.size() * sizeof(uint64_t),
						stageResults.data(), sizeof(uint64_t),
						VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

					if (res == VK_SUCCESS) {
						float durationMS = 0.0f;
						if (stageResults[1] >= stageResults[0]) {
							durationMS = static_cast<float>(stageResults[1] - stageResults[0]) *
										 timestampPeriod / 1000000.0f;
						}
						callback(Stage::name, durationMS);
					}
				}
			}.template operator()<Stages>(),
			...);
	}

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
