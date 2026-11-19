// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "vulkan/diagnostics/GpuProfiler.hpp"
#include <Zahlen/Core/Reflection/Enums.hpp>

namespace ZHLN::Profiler {

template <typename EnumT>
    requires std::is_enum_v<EnumT>
inline void GpuProfiler<EnumT>::Teardown() noexcept {
    if (_device != VK_NULL_HANDLE) {
        for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
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
    _device(std::exchange(other._device, VK_NULL_HANDLE)), _pools(std::exchange(other._pools, {})),
    _recordedMasks(std::exchange(other._recordedMasks, {})), _enabled(std::exchange(other._enabled, false)),
    _statsPools(std::exchange(other._statsPools, {})), _statsBeginMasks(std::exchange(other._statsBeginMasks, {})),
    _statsEndMasks(std::exchange(other._statsEndMasks, {})), _statsBits(std::exchange(other._statsBits, 0)),
    _statsSupported(std::exchange(other._statsSupported, false)), _statsEnabled(std::exchange(other._statsEnabled, false)) {
}

template <typename EnumT>
    requires std::is_enum_v<EnumT>
inline auto GpuProfiler<EnumT>::operator=(GpuProfiler&& other) noexcept -> GpuProfiler& {
    if (this != &other) {
        Teardown();
        _device          = std::exchange(other._device, VK_NULL_HANDLE);
        _pools           = std::exchange(other._pools, {});
        _recordedMasks   = std::exchange(other._recordedMasks, {});
        _enabled         = std::exchange(other._enabled, false);
        _statsPools      = std::exchange(other._statsPools, {});
        _statsBeginMasks = std::exchange(other._statsBeginMasks, {});
        _statsEndMasks   = std::exchange(other._statsEndMasks, {});
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
    _recordedMasks = {};
    _enabled       = false;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    if (props.limits.timestampPeriod == 0) {

        return {};
    }

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queue_family_count, queue_families.data());

    if (queueFamilyIndex >= queue_family_count || queue_families[queueFamilyIndex].timestampValidBits == 0) {
        return {};
    }

    VkQueryPoolCreateInfo info = {
        .sType              = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .pNext              = nullptr,
        .flags              = 0,
        .queryType          = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount         = kQueryCount,
        .pipelineStatistics = 0
    };

    for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
        if (vkCreateQueryPool(device, &info, nullptr, &_pools[i]) != VK_SUCCESS) {

            _pools[i] = VK_NULL_HANDLE;
            Teardown();
            return std::unexpected(GpuProfilerError::QueryPoolCreationFailed);
        }

        vkResetQueryPool(device, _pools[i], 0, kQueryCount);
    }

    _enabled = true;

    VkPhysicalDeviceFeatures deviceFeatures {};
    vkGetPhysicalDeviceFeatures(physicalDevice, &deviceFeatures);
    if (deviceFeatures.pipelineStatisticsQuery != VK_TRUE) {
        return {};
    }

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
        .queryCount         = kStageCount,
        .pipelineStatistics = _statsBits
    };

    for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
        if (vkCreateQueryPool(device, &statsInfo, nullptr, &_statsPools[i]) != VK_SUCCESS) {

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
    uint32_t slot = Vk::FrameSlot(frameIndex);
    vkResetQueryPool(_device, _pools[slot], 0, kQueryCount);
    _recordedMasks[slot].Reset();

    if (_statsSupported) {
        vkResetQueryPool(_device, _statsPools[slot], 0, kStageCount);
    }

    _statsBeginMasks[slot].Reset();
    _statsEndMasks[slot].Reset();
}

template <typename EnumT>
    requires std::is_enum_v<EnumT>
void GpuProfiler<EnumT>::WriteStart(VkCommandBuffer cmd, uint32_t frameIndex, EnumT stage) const noexcept {
    if (!_enabled || !ValidStage(stage)) {
        return;
    }

    const uint32_t slot = Vk::FrameSlot(frameIndex);
    _recordedMasks[slot].Set(stage);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_NONE, _pools[slot], TimestampStartQuery(stage));

    if (_statsEnabled) {
        vkCmdBeginQuery(cmd, _statsPools[slot], StatsQuery(stage), 0);
        _statsBeginMasks[slot].Set(stage);
    }
}

template <typename EnumT>
    requires std::is_enum_v<EnumT>
void GpuProfiler<EnumT>::WriteEnd(VkCommandBuffer cmd, uint32_t frameIndex, EnumT stage) const noexcept {
    if (!_enabled || !ValidStage(stage)) {
        return;
    }

    const uint32_t slot = Vk::FrameSlot(frameIndex);

    if (_statsSupported && _statsBeginMasks[slot].Contains(stage)) {
        vkCmdEndQuery(cmd, _statsPools[slot], StatsQuery(stage));
        _statsEndMasks[slot].Set(stage);
    }

    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_NONE, _pools[slot], TimestampEndQuery(stage));
}

template <typename EnumT>
    requires std::is_enum_v<EnumT>
template <typename Func>
inline void GpuProfiler<EnumT>::RetrieveResults(uint32_t frameIndex, float timestampPeriod, Func&& callback) noexcept {
    if (!_enabled) {
        return;
    }
    const uint32_t          slot = Vk::FrameSlot(frameIndex);
    const EnumBitMask<EnumT> mask = _recordedMasks[slot];
    if (mask.Empty()) {
        return;
    }

    VkQueryPool pool = _pools[slot];

    Reflect::ForEachEnumerator<EnumT>([&]<EnumT StageVal>() -> void {
        if (!mask.Contains(StageVal)) {
            return;
        }

        std::array<uint64_t, 2> stage_results {};
        const VkResult          res = vkGetQueryPoolResults(
            _device, pool, TimestampStartQuery(StageVal), 2,
            stage_results.size() * sizeof(uint64_t), stage_results.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT
        );

        if (res == VK_SUCCESS) {
            float duration_ms = 0.0F;
            if (stage_results[1] >= stage_results[0]) {
                duration_ms = static_cast<float>(stage_results[1] - stage_results[0]) * timestampPeriod / 1000000.0F;
            }
            constexpr std::string_view name = Reflect::EnumToString(StageVal);
            callback(name, duration_ms);
        }
    });
    _recordedMasks[slot].Reset();
}

template <typename EnumT>
    requires std::is_enum_v<EnumT>
template <typename Func>
inline void GpuProfiler<EnumT>::RetrievePipelineStats(uint32_t frameIndex, Func&& callback) noexcept {
    if (!_statsSupported) {
        return;
    }
    const uint32_t slot = Vk::FrameSlot(frameIndex);

    const EnumBitMask<EnumT> completed = _statsBeginMasks[slot] & _statsEndMasks[slot];
    if (completed.Empty()) {
        return;
    }

    const uint32_t resultCount = static_cast<uint32_t>(std::popcount(_statsBits));
    VkQueryPool    pool        = _statsPools[slot];

    Reflect::ForEachEnumerator<EnumT>([&]<EnumT StageVal>() -> void {
        if (!completed.Contains(StageVal)) {
            return;
        }

        std::array<uint64_t, 11> results {};
        const VkResult           res = vkGetQueryPoolResults(
            _device, pool, StatsQuery(StageVal), 1,
            results.size() * sizeof(uint64_t), results.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT
        );
        if (res != VK_SUCCESS) {
            return;
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

        constexpr std::string_view name = Reflect::EnumToString(StageVal);
        callback(name, stats);
    });
    _statsBeginMasks[slot].Reset();
    _statsEndMasks[slot].Reset();
}

template <typename EnumT>
inline ScopedGpuProfile<EnumT>::ScopedGpuProfile(VkCommandBuffer cmd, uint32_t frameIndex, const GpuProfiler<EnumT>& profiler, EnumT stage) noexcept:
    _cmd(cmd), _frameIndex(frameIndex), _profiler(profiler), _stage(stage) {
    _profiler.WriteStart(_cmd, _frameIndex, _stage);
}

template <typename EnumT>
inline ScopedGpuProfile<EnumT>::~ScopedGpuProfile() noexcept {
    _profiler.WriteEnd(_cmd, _frameIndex, _stage);
}

}
