// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Postprocessing.inl
#pragma once

#include "Postprocessing.hpp"

namespace ZHLN::Vk {

template <typename LayoutT>
bool PostProcessPass<LayoutT>::BuildHeap(
    VkDevice device, HeapManager& heap, const ShaderStages& shaders, std::initializer_list<VkFormat> colorFormats, bool additive
) noexcept {
    // Reflection only: the binding structure drives the mapping table.
    if (!layoutInstance.Build(device, shaders)) {
        return false;
    }

    BuildHeapPassBindings(heap, layoutInstance.reflectedSets[0], 0, kHeapIndexPushOffset, 2, heapBindings);

    auto builder = PipelineBuilder {}
                       .Shaders(shaders)
                       .Layout(VK_NULL_HANDLE)
                       .HeapMappings(heapBindings.GetInfo(), heapBindings.GetInfo())
                       .ColorFormats(colorFormats)
                       .NoDepth()
                       .CullNone();
    if (additive) {
        builder.AdditiveBlend();
    }

    auto p_res = builder.Build(device);
    if (!p_res) {
        return false;
    }
    pipeline = std::move(*p_res);
    return true;
}

template <typename LayoutT>
bool PostProcessPass<LayoutT>::BuildHeapVariants(
    VkDevice device, HeapManager& heap, const ShaderStages& shaders, std::initializer_list<VkFormat> colorFormats,
    std::span<const VkSpecializationInfo> specInfos, bool additive
) noexcept {
    // Specialization does not change the descriptor interface, so one mapping
    // table covers every variant.
    if (!layoutInstance.Build(device, shaders)) {
        return false;
    }

    BuildHeapPassBindings(heap, layoutInstance.reflectedSets[0], 0, kHeapIndexPushOffset, 2, heapBindings);

    pipelines.clear();
    pipelines.reserve(specInfos.size());

    for (const auto& spec: specInfos) {
        auto builder = PipelineBuilder {}
                           .Shaders(shaders)
                           .Layout(VK_NULL_HANDLE)
                           .HeapMappings(heapBindings.GetInfo(), heapBindings.GetInfo())
                           .ColorFormats(colorFormats)
                           .Specialization(&spec)
                           .NoDepth()
                           .CullNone();
        if (additive) {
            builder.AdditiveBlend();
        }

        auto p_res = builder.Build(device);
        if (!p_res) {
            return false;
        }
        pipelines.push_back(std::move(*p_res));
    }

    return !pipelines.empty();
}

template <typename LayoutT>
template <typename... Args>
void PostProcessPass<LayoutT>::WriteHeap(const Context& ctx, HeapManager& heap, uint32_t heapIndex, Args&&... args) const noexcept {
    WriteHeapBindings(heap, ctx, heapBindings, heapIndex, std::forward<Args>(args)...);
}

template <typename LayoutT>
template <GpuTriviallyCopyable T>
void PostProcessPass<LayoutT>::ExecuteHeap(const Context& ctx, VkCommandBuffer cmd, const T& pushData, uint32_t heapIndex, VkShaderStageFlags stages)
    const noexcept {
    static_assert(sizeof(T) <= kHeapIndexPushOffset, "Pass push struct overruns the descriptor-index word");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
    PushData(ctx, cmd, 0, pushData);
    PushHeapIndex(ctx, cmd, kHeapIndexPushOffset, heapIndex);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

template <typename LayoutT>
template <GpuTriviallyCopyable T>
void PostProcessPass<LayoutT>::ExecuteVariantHeap(
    const Context& ctx, VkCommandBuffer cmd, uint32_t variantIdx, const T& pushData, uint32_t heapIndex, VkShaderStageFlags stages
) const noexcept {
    static_assert(sizeof(T) <= kHeapIndexPushOffset, "Pass push struct overruns the descriptor-index word");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[variantIdx].Get());
    PushData(ctx, cmd, 0, pushData);
    PushHeapIndex(ctx, cmd, kHeapIndexPushOffset, heapIndex);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

template <typename LayoutT>
void PostProcessPass<LayoutT>::ExecuteHeap(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex) const noexcept {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
    PushHeapIndex(ctx, cmd, kHeapIndexPushOffset, heapIndex);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace ZHLN::Vk
