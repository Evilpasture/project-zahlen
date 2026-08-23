// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/GpuProfiler.inl

#pragma once
#include "GpuProfiler.hpp"
#include <Zahlen/Core/Reflection.hpp>

namespace ZHLN::Profiler {

// ============================================================================
// GpuProfiler Implementation
// ============================================================================

template <typename EnumT>
    requires std::is_enum_v<EnumT>
inline GpuProfiler<EnumT>::~GpuProfiler() noexcept {
    if (_device != VK_NULL_HANDLE) {
        for (uint32_t i = 0; i < 2; ++i) {
            if (_pools[i] != VK_NULL_HANDLE) {
                vkDestroyQueryPool(_device, _pools[i], nullptr);
            }
        }
    }
}

template <typename EnumT>
    requires std::is_enum_v<EnumT>
inline GpuProfiler<EnumT>::GpuProfiler(GpuProfiler&& other) noexcept:
    _device(std::exchange(other._device, VK_NULL_HANDLE)), _pools(std::exchange(other._pools, {VK_NULL_HANDLE, VK_NULL_HANDLE})),
    _recordedMasks(std::exchange(other._recordedMasks, {0, 0})), _enabled(std::exchange(other._enabled, false)) {
}

template <typename EnumT>
    requires std::is_enum_v<EnumT>
inline auto GpuProfiler<EnumT>::operator=(GpuProfiler&& other) noexcept -> GpuProfiler& {
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

template <typename EnumT>
    requires std::is_enum_v<EnumT>
inline void GpuProfiler<EnumT>::Init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex) noexcept {
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

template <typename EnumT>
    requires std::is_enum_v<EnumT>
inline void GpuProfiler<EnumT>::Reset(uint32_t frameIndex) noexcept {
    if (!_enabled) {
        return;
    }
    uint32_t slot = frameIndex % 2;
    vkResetQueryPool(_device, _pools[slot], 0, kQueryCount);
    _recordedMasks[slot] = 0;
}

template <typename EnumT>
    requires std::is_enum_v<EnumT>
void GpuProfiler<EnumT>::WriteStart(VkCommandBuffer cmd, uint32_t frameIndex, EnumT stage) const noexcept {
    if (!_enabled) {
        return;
    }
    auto     stage_idx = static_cast<uint32_t>(stage);
    uint32_t query_idx = stage_idx * 2;

    const uint32_t slot = frameIndex % static_cast<uint32_t>(_pools.size());
    _recordedMasks[slot] |= (uint64_t {1} << stage_idx);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_NONE, _pools[slot], query_idx);
}

template <typename EnumT>
    requires std::is_enum_v<EnumT>
void GpuProfiler<EnumT>::WriteEnd(VkCommandBuffer cmd, uint32_t frameIndex, EnumT stage) const noexcept {
    if (!_enabled) {
        return;
    }
    auto     stage_idx = static_cast<uint32_t>(stage);
    uint32_t query_idx = (stage_idx * 2) + 1;

    const uint32_t slot = frameIndex % static_cast<uint32_t>(_pools.size());
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_NONE, _pools[slot], query_idx);
}

template <typename EnumT>
    requires std::is_enum_v<EnumT>
template <typename Func>
inline void GpuProfiler<EnumT>::RetrieveResults(uint32_t frameIndex, float timestampPeriod, Func&& callback) noexcept {
    if (!_enabled) {
        return;
    }
    uint32_t slot = frameIndex % 2;
    uint64_t mask = _recordedMasks[slot];
    if (mask == 0) {
        return;
    }

    VkQueryPool pool = _pools[slot];

    for (uint32_t i = 0; i < kStageCount; ++i) {
        if (mask & (uint64_t {1} << i)) {
            uint32_t                start_idx = i * 2;
            std::array<uint64_t, 2> stage_results {};

            VkResult res = vkGetQueryPoolResults(
                _device, pool, start_idx, 2, stage_results.size() * sizeof(uint64_t), stage_results.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT
            );

            if (res == VK_SUCCESS) {
                float duration_ms = 0.0F;
                if (stage_results[1] >= stage_results[0]) {
                    duration_ms = static_cast<float>(stage_results[1] - stage_results[0]) * timestampPeriod / 1000000.0F;
                }
                auto             stage_enum = static_cast<EnumT>(i);
                std::string_view name       = Reflect::EnumToString(stage_enum);
                std::forward<Func>(callback)(name, duration_ms);
            }
        }
    }
    _recordedMasks[slot] = 0;
}

// ============================================================================
// ScopedGpuProfile Implementation
// ============================================================================

template <typename EnumT>
inline ScopedGpuProfile<EnumT>::ScopedGpuProfile(VkCommandBuffer cmd, uint32_t frameIndex, const GpuProfiler<EnumT>& profiler, EnumT stage) noexcept:
    _cmd(cmd), _frameIndex(frameIndex), _profiler(profiler), _stage(stage) {
    _profiler.WriteStart(_cmd, _frameIndex, _stage);
}

template <typename EnumT>
inline ScopedGpuProfile<EnumT>::~ScopedGpuProfile() noexcept {
    _profiler.WriteEnd(_cmd, _frameIndex, _stage);
}

} // namespace ZHLN::Profiler
