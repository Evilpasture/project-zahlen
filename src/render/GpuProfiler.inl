// src/render/GpuProfiler.inl

#pragma once
#include "GpuProfiler.hpp"

namespace ZHLN::Profiler {

// ============================================================================
// GpuProfiler Implementation
// ============================================================================

template <GpuStageTag... Stages>
inline GpuProfiler<Stages...>::~GpuProfiler() noexcept {
    if (_device != VK_NULL_HANDLE) {
        for (uint32_t i = 0; i < 2; ++i) {
            if (_pools[i] != VK_NULL_HANDLE) {
                vkDestroyQueryPool(_device, _pools[i], nullptr);
            }
        }
    }
}

template <GpuStageTag... Stages>
inline GpuProfiler<Stages...>::GpuProfiler(GpuProfiler&& other) noexcept:
    _device(std::exchange(other._device, VK_NULL_HANDLE)), _pools(std::exchange(other._pools, {VK_NULL_HANDLE, VK_NULL_HANDLE})),
    _recordedMasks(std::exchange(other._recordedMasks, {0, 0})), _enabled(std::exchange(other._enabled, false)) {
}

template <GpuStageTag... Stages>
inline auto GpuProfiler<Stages...>::operator=(GpuProfiler&& other) noexcept -> GpuProfiler& {
    if (this != &other) {
        if (_device != VK_NULL_HANDLE) {
            for (uint32_t i = 0; i < 2; ++i) {
                if (_pools[i] != VK_NULL_HANDLE) {
                    vkDestroyQueryPool(_device, _pools[i], nullptr);
                }
            }
        }
        _device        = std::exchange(other._device, VK_NULL_HANDLE);
        _pools         = std::exchange(other._pools, {VK_NULL_HANDLE, VK_NULL_HANDLE});
        _recordedMasks = std::exchange(other._recordedMasks, {0, 0});
        _enabled       = std::exchange(other._enabled, false);
    }
    return *this;
}

template <GpuStageTag... Stages>
inline void GpuProfiler<Stages...>::Init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex) noexcept {
    _device        = device;
    _recordedMasks = {0, 0};
    _enabled       = false;

    // 1. Query physical device limits to verify timestamp support
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    if (props.limits.timestampPeriod == 0) {
        return; // Timestamp queries not supported by hardware limits
    }

    // 2. Query queue family properties to verify valid bits
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queue_family_count, queue_families.data());

    if (queueFamilyIndex >= queue_family_count || queue_families[queueFamilyIndex].timestampValidBits == 0) {
        return; // Queue family does not support timestamps
    }

    _enabled = true;

    VkQueryPoolCreateInfo info = {
        .sType              = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .pNext              = nullptr,
        .flags              = 0,
        .queryType          = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount         = kQueryCount,
        .pipelineStatistics = 0
    };

    for (uint32_t i = 0; i < 2; ++i) {
        vkCreateQueryPool(device, &info, nullptr, &_pools[i]);
        vkResetQueryPool(device, _pools[i], 0, kQueryCount);
    }
}

template <GpuStageTag... Stages>
inline void GpuProfiler<Stages...>::Reset(uint32_t frameIndex) noexcept {
    if (!_enabled) {
        return;
    }
    uint32_t slot = frameIndex % 2;
    vkResetQueryPool(_device, _pools[slot], 0, kQueryCount);
    _recordedMasks[slot] = 0;
}

template <GpuStageTag... Stages>
template <GpuStageTag Stage>
inline void GpuProfiler<Stages...>::WriteStart(VkCommandBuffer cmd, uint32_t frameIndex) const noexcept {
    if (!_enabled) {
        return;
    }
    static_assert(ContainsType<Stage, Stages...>, "Stage tag not registered in this GpuProfiler!");
    constexpr uint32_t stage_idx = TypeIndex<Stage, Stages...>::value;
    constexpr uint32_t query_idx = stage_idx * 2;

    uint32_t slot = frameIndex % 2;
    _recordedMasks[slot] |= (1U << stage_idx);

    // --- FIXED: Detect if this is a Compute pass and use the optimal stage mask to prevent hardware deadlocks ---
    constexpr std::string_view stage_name = Stage::name;
    constexpr bool             is_compute = stage_name.contains("Volumetric") || stage_name.contains("Compute");
    VkPipelineStageFlags2      stage_mask = is_compute ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

    vkCmdWriteTimestamp2(cmd, stage_mask, _pools[slot], query_idx);
}

template <GpuStageTag... Stages>
template <GpuStageTag Stage>
inline void GpuProfiler<Stages...>::WriteEnd(VkCommandBuffer cmd, uint32_t frameIndex) const noexcept {
    if (!_enabled) {
        return;
    }
    static_assert(ContainsType<Stage, Stages...>, "Stage tag not registered in this GpuProfiler!");
    constexpr uint32_t query_idx = (TypeIndex<Stage, Stages...>::value * 2) + 1;

    // --- FIXED: Detect if this is a Compute pass and use the optimal stage mask to prevent hardware deadlocks ---
    constexpr std::string_view stage_name = Stage::name;
    constexpr bool             is_compute = stage_name.contains("Volumetric") || stage_name.contains("Compute");
    VkPipelineStageFlags2      stage_mask = is_compute ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

    vkCmdWriteTimestamp2(cmd, stage_mask, _pools[frameIndex % 2], query_idx);
}

template <GpuStageTag... Stages>
template <typename Func>
inline void GpuProfiler<Stages...>::RetrieveResults(uint32_t frameIndex, float timestampPeriod, Func&& callback) noexcept {
    if (!_enabled) {
        return;
    }
    uint32_t slot = frameIndex % 2;
    uint32_t mask = _recordedMasks[slot];
    if (mask == 0) {
        return;
    }

    VkQueryPool pool = _pools[slot];

    (
        [&]<typename Stage>() {
            constexpr uint32_t stage_idx = TypeIndex<Stage, Stages...>::value;
            if (mask & (1U << stage_idx)) {
                constexpr uint32_t      start_idx = stage_idx * 2;
                std::array<uint64_t, 2> stage_results {};

                // Removed VK_QUERY_RESULT_WAIT_BIT to prevent hard CPU execution stalls
                VkResult res = vkGetQueryPoolResults(
                    _device, pool, start_idx, 2, stage_results.size() * sizeof(uint64_t), stage_results.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT
                );

                if (res == VK_SUCCESS) {
                    float duration_ms = 0.0F;
                    if (stage_results[1] >= stage_results[0]) {
                        duration_ms = static_cast<float>(stage_results[1] - stage_results[0]) * timestampPeriod / 1000000.0F;
                    }
                    std::forward<Func>(callback)(Stage::name, duration_ms);
                }
            }
        }.template operator()<Stages>(),
        ...);
}

// ============================================================================
// ScopedGpuProfile Implementation
// ============================================================================

template <GpuStageTag Stage, typename ProfilerT>
inline ScopedGpuProfile<Stage, ProfilerT>::ScopedGpuProfile(VkCommandBuffer cmd, uint32_t frameIndex, const ProfilerT& profiler) noexcept:
    _cmd(cmd), _frameIndex(frameIndex), _profiler(profiler) {
    _profiler.template WriteStart<Stage>(_cmd, _frameIndex);
}

template <GpuStageTag Stage, typename ProfilerT>
inline ScopedGpuProfile<Stage, ProfilerT>::~ScopedGpuProfile() noexcept {
    _profiler.template WriteEnd<Stage>(_cmd, _frameIndex);
}

} // namespace ZHLN::Profiler
