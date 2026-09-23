// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/diagnostics/GpuProfiler.inl

#pragma once
#include "GpuProfiler.hpp"
#include <Zahlen/Core/Reflection/Enums.hpp>

namespace ZHLN::Profiler {

// GpuProfiler Implementation

template <typename EnumT>
    requires std::is_enum_v<EnumT>
inline void GpuProfiler<EnumT>::Teardown() noexcept {
    if (_device != VK_NULL_HANDLE) {
        for (uint32_t i = 0; i < 2; ++i) {
            if (_pools[i] != VK_NULL_HANDLE) {
                vkDestroyQueryPool(_device, _pools[i], nullptr);
                _pools[i] = VK_NULL_HANDLE;
            }
            if (_statsPools[i] != VK_NULL_HANDLE) {
                vkDestroyQueryPool(_device, _statsPools[i], nullptr);
                _statsPools[i] = VK_NULL_HANDLE;
            }
        }
        _device = VK_NULL_HANDLE;
    }
}

template <typename EnumT>
    requires std::is_enum_v<EnumT>
inline GpuProfiler<EnumT>::~GpuProfiler() noexcept {
    Teardown();
}

template <typename EnumT>
    requires std::is_enum_v<EnumT>
inline GpuProfiler<EnumT>::GpuProfiler(GpuProfiler&& other) noexcept:
    _device(std::exchange(other._device, VK_NULL_HANDLE)), _pools(std::exchange(other._pools, {VK_NULL_HANDLE, VK_NULL_HANDLE})),
    _recordedMasks(std::exchange(other._recordedMasks, {0, 0})), _enabled(std::exchange(other._enabled, false)),
    _statsPools(std::exchange(other._statsPools, {VK_NULL_HANDLE, VK_NULL_HANDLE})), _statsBeginMasks(std::exchange(other._statsBeginMasks, {0, 0})),
    _statsEndMasks(std::exchange(other._statsEndMasks, {0, 0})), _statsBits(std::exchange(other._statsBits, 0)),
    _statsSupported(std::exchange(other._statsSupported, false)), _statsEnabled(std::exchange(other._statsEnabled, false)) {
}

template <typename EnumT>
    requires std::is_enum_v<EnumT>
inline auto GpuProfiler<EnumT>::operator=(GpuProfiler&& other) noexcept -> GpuProfiler& {
    if (this != &other) {
        Teardown();
        _device          = std::exchange(other._device, VK_NULL_HANDLE);
        _pools           = std::exchange(other._pools, {VK_NULL_HANDLE, VK_NULL_HANDLE});
        _recordedMasks   = std::exchange(other._recordedMasks, {0, 0});
        _enabled         = std::exchange(other._enabled, false);
        _statsPools      = std::exchange(other._statsPools, {VK_NULL_HANDLE, VK_NULL_HANDLE});
        _statsBeginMasks = std::exchange(other._statsBeginMasks, {0, 0});
        _statsEndMasks   = std::exchange(other._statsEndMasks, {0, 0});
        _statsBits       = std::exchange(other._statsBits, 0);
        _statsSupported  = std::exchange(other._statsSupported, false);
        _statsEnabled    = std::exchange(other._statsEnabled, false);
    }
    return *this;
}

template <typename EnumT>
    requires std::is_enum_v<EnumT>
inline auto GpuProfiler<EnumT>::Init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, bool meshPipelineStats) noexcept
    -> std::expected<void, ErrorCode> {
    _device        = device;
    _recordedMasks = {0, 0};
    _enabled       = false;

    // 1. Query physical device limits to verify timestamp support
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    if (props.limits.timestampPeriod == 0) {
        // Timestamp queries not supported by hardware limits. Success with the
        // profiler left disabled: this is absent capability, not a failure.
        return {};
    }

    // 2. Query queue family properties to verify valid bits
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queue_family_count, queue_families.data());

    if (queueFamilyIndex >= queue_family_count || queue_families[queueFamilyIndex].timestampValidBits == 0) {
        return {}; // Queue family does not support timestamps
    }

    VkQueryPoolCreateInfo info = {
        .sType              = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .pNext              = nullptr,
        .flags              = 0,
        .queryType          = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount         = kQueryCount,
        .pipelineStatistics = 0
    };

    for (uint32_t i = 0; i < 2; ++i) {
        if (vkCreateQueryPool(device, &info, nullptr, &_pools[i]) != VK_SUCCESS) {
            // pQueryPool is undefined on failure; restore the null invariant so
            // Teardown() does not destroy a garbage handle.
            _pools[i] = VK_NULL_HANDLE;
            Teardown();
            return std::unexpected(GpuProfilerError::QueryPoolCreationFailed);
        }
        // Returns void: host query reset cannot fail, so there is nothing here
        // to report.
        vkResetQueryPool(device, _pools[i], 0, kQueryCount);
    }

    // Only now: previously this was set before the pools existed, so a failed
    // creation left the profiler "enabled" with null pools and Reset() went on
    // to call vkResetQueryPool on VK_NULL_HANDLE.
    _enabled = true;

    // 3. Pipeline statistics pools: opt-in counters (see header). Absent
    // support is not an error -- same policy as timestamps above. The feature
    // bit itself is enabled at device creation (RenderInitDevice) when the
    // physical device advertises it, which is exactly the condition probed
    // here, so a supported device arrives with the feature on.
    VkPhysicalDeviceFeatures deviceFeatures {};
    vkGetPhysicalDeviceFeatures(physicalDevice, &deviceFeatures);
    if (deviceFeatures.pipelineStatisticsQuery != VK_TRUE) {
        return {}; // Statistics queries unavailable; timestamps keep working.
    }

    // The nine core statistic bits are valid on any device with the
    // feature. The task/mesh bits are what measure meshlet culling, but they
    // are legal only when the device has meshShaderQueries ENABLED
    // (VUID-VkQueryPoolCreateInfo-meshShaderQueries-07069) -- enablement, not
    // physical-device support, is what the VUID checks, so the caller passes
    // the device-creation state instead of us probing here.
    _statsBits = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                 VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
                 VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
                 VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
                 VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;

    if (meshPipelineStats) {
        _statsBits |= VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT | VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT;
    }

    VkQueryPoolCreateInfo statsInfo = {
        .sType              = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .pNext              = nullptr,
        .flags              = 0,
        .queryType          = VK_QUERY_TYPE_PIPELINE_STATISTICS,
        .queryCount         = kStageCount, // one query per stage: it spans the Begin/End pair
        .pipelineStatistics = _statsBits
    };

    for (uint32_t i = 0; i < 2; ++i) {
        if (vkCreateQueryPool(device, &statsInfo, nullptr, &_statsPools[i]) != VK_SUCCESS) {
            // Same invariant as the timestamp pools: never leave a pool the
            // creation refused in a slot Teardown() would destroy.
            _statsPools[i] = VK_NULL_HANDLE;
            Teardown();
            return std::unexpected(GpuProfilerError::StatsQueryPoolCreationFailed);
        }
        vkResetQueryPool(device, _statsPools[i], 0, kStageCount);
    }

    _statsSupported = true;
    return {};
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

    if (_statsSupported) {
        vkResetQueryPool(_device, _statsPools[slot], 0, kStageCount);
    }
    // Masks reset unconditionally: a toggle-off must not leave stale bits
    // that a later toggle-on would misread as recorded scopes.
    _statsBeginMasks[slot] = 0;
    _statsEndMasks[slot]   = 0;
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

    // Scope placement: the render graph records these calls OUTSIDE any
    // render-pass instance (the timestamps require it), which also satisfies
    // the pipeline-statistics rule that one query's Begin and End must not
    // straddle a render pass.
    if (_statsEnabled) {
        vkCmdBeginQuery(cmd, _statsPools[slot], stage_idx, 0);
        _statsBeginMasks[slot] |= (uint64_t {1} << stage_idx);
    }
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

    // Close whatever Begin opened, regardless of the CURRENT toggle state: an
    // active query left unterminated is a validation error, so a toggle-off
    // mid-frame must not strand the scopes already begun.
    if (_statsSupported && (_statsBeginMasks[slot] & (uint64_t {1} << stage_idx)) != 0) {
        vkCmdEndQuery(cmd, _statsPools[slot], stage_idx);
        _statsEndMasks[slot] |= (uint64_t {1} << stage_idx);
    }

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

template <typename EnumT>
    requires std::is_enum_v<EnumT>
template <typename Func>
inline void GpuProfiler<EnumT>::RetrievePipelineStats(uint32_t frameIndex, Func&& callback) noexcept {
    if (!_statsSupported) {
        return;
    }
    const uint32_t slot = frameIndex % 2;
    // Only complete Begin/End pairs carry defined results; an enabling toggle
    // mid-frame leaves begun-but-unterminated scopes out of this mask.
    const uint64_t completed = _statsBeginMasks[slot] & _statsEndMasks[slot];
    if (completed == 0) {
        return;
    }

    const uint32_t resultCount = static_cast<uint32_t>(std::popcount(_statsBits));
    VkQueryPool    pool        = _statsPools[slot];

    for (uint32_t i = 0; i < kStageCount; ++i) {
        if ((completed & (uint64_t {1} << i)) == 0) {
            continue;
        }

        // One uint64 per queried statistic bit, densely packed in ascending
        // bit order (Vulkan result layout for pipeline statistics queries).
        std::array<uint64_t, 11> results {}; // 9 core statistic bits + 2 VK_EXT_mesh_shader bits
        const VkResult           res =
            vkGetQueryPoolResults(_device, pool, i, 1, results.size() * sizeof(uint64_t), results.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (res != VK_SUCCESS) {
            continue;
        }

        PipelineStats stats {};
        uint32_t      k    = 0;
        const auto    take = [&](VkQueryPipelineStatisticFlagBits bit, uint64_t PipelineStats::* field) {
            if ((_statsBits & bit) != 0 && k < resultCount) {
                stats.*field = results[k++];
            }
        };
        take(VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT, &PipelineStats::iaVertices);
        take(VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT, &PipelineStats::iaPrimitives);
        take(VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT, &PipelineStats::vsInvocations);
        take(VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT, &PipelineStats::clipperInvocations);
        take(VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT, &PipelineStats::clipperPrimitivesOut);
        take(VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT, &PipelineStats::gsInvocations);
        take(VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT, &PipelineStats::gsPrimitives);
        take(VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT, &PipelineStats::fsInvocations);
        take(VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT, &PipelineStats::csInvocations);
        take(VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT, &PipelineStats::taskInvocations);
        take(VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT, &PipelineStats::meshInvocations);

        const auto stageEnum = static_cast<EnumT>(i);
        std::forward<Func>(callback)(Reflect::EnumToString(stageEnum), stats);
    }
    _statsBeginMasks[slot] = 0;
    _statsEndMasks[slot]   = 0;
}

// ScopedGpuProfile Implementation

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
