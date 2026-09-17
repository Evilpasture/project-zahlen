// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/Postprocessing.inl
#pragma once

#include "Postprocessing.hpp"

namespace ZHLN::Vk {

template <typename LayoutT>
bool PostProcessPass<LayoutT>::BuildHeap(
    VkDevice                        device,
    HeapManager&                    heap,
    const ShaderStages&             shaders,
    std::initializer_list<VkFormat> colorFormats,
    uint32_t                        indexPushOffset,
    HeapLifecycle                   lifecycle,
    bool                            additive,
    VkPipelineCache                 cache
) noexcept {
    // Reflection only: the binding structure drives the mapping table.
    if (!layoutInstance.Build(device, shaders)) {
        return false;
    }

    if (!BuildHeapPassBindings(heap, layoutInstance.sets[0], 0, indexPushOffset, lifecycle, heapBindings)) {
        return false;
    }

    auto builder = PipelineBuilder {}
                       .Shaders(shaders)
                       .Layout(VK_NULL_HANDLE)
                       .Cache(cache)
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
    VkDevice                              device,
    HeapManager&                          heap,
    const ShaderStages&                   shaders,
    std::initializer_list<VkFormat>       colorFormats,
    std::span<const VkSpecializationInfo> specInfos,
    uint32_t                              indexPushOffset,
    HeapLifecycle                         lifecycle,
    bool                                  additive,
    VkPipelineCache                       cache
) noexcept {
    // Specialization does not change the descriptor interface, so one mapping
    // table covers every variant.
    if (!layoutInstance.Build(device, shaders)) {
        return false;
    }

    if (!BuildHeapPassBindings(heap, layoutInstance.sets[0], 0, indexPushOffset, lifecycle, heapBindings)) {
        return false;
    }

    pipelines.clear();
    pipelines.reserve(specInfos.size());

    for (const auto& spec: specInfos) {
        auto builder = PipelineBuilder {}
                           .Shaders(shaders)
                           .Layout(VK_NULL_HANDLE)
                           .Cache(cache)
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
template <typename... Slots>
auto PostProcessPass<LayoutT>::WriteHeapParameters(const Context& ctx, HeapManager& heap, const Slots&... slots) const noexcept -> HeapBlockBase {
    return heap.WriteHeapParameters(ctx, heapBindings, slots...);
}

template <typename LayoutT>
template <PostProcessPushPayload T>
void PostProcessPass<LayoutT>::ExecuteHeap(const Context& ctx, VkCommandBuffer cmd, const T& pushData, HeapBlockBase blockBase) const noexcept {
    static_assert(sizeof(T) <= kScenePassPushPayloadBytes, "Pass push struct exceeds DescriptorHeapPushData::passData.");
    ZHLN::Assert(cmd != VK_NULL_HANDLE, "{} requires a valid VkCommandBuffer.", "post-process fullscreen draw");
    ZHLN::Assert(Valid(), "Attempted to bind an invalid post-process pipeline.");
    ZHLN::Assert(heapBindings.indexPushOffset > 0, "Missing reflected descriptor-index offset.");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
    PushData(ctx, cmd, 0, pushData);
    // The mapping is slot-independent: what travels here is the block's base
    // slot, not an ordinal.
    PushHeapIndex(ctx, cmd, heapBindings.indexPushOffset, blockBase.slot);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

template <typename LayoutT>
template <PostProcessPushPayload T>
void PostProcessPass<LayoutT>::ExecuteVariantHeap(
    const Context&  ctx,
    VkCommandBuffer cmd,
    uint32_t        variantIdx,
    const T&        pushData,
    HeapBlockBase   blockBase
) const noexcept {
    static_assert(sizeof(T) <= kScenePassPushPayloadBytes, "Pass push struct exceeds DescriptorHeapPushData::passData.");
    ZHLN::Assert(cmd != VK_NULL_HANDLE, "{} requires a valid VkCommandBuffer.", "post-process fullscreen draw");
    ZHLN::Assert(heapBindings.indexPushOffset > 0, "Missing reflected descriptor-index offset.");
    ZHLN::Assert(variantIdx < pipelines.size(), "Post-process pipeline variant index {} is out of bounds ({} variants).", variantIdx, pipelines.size());
    ZHLN::Assert(pipelines[variantIdx].Valid(), "Attempted to bind an invalid post-process pipeline variant {}.", variantIdx);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[variantIdx].Get());
    PushData(ctx, cmd, 0, pushData);
    // The mapping is slot-independent: what travels here is the block's base
    // slot, not an ordinal.
    PushHeapIndex(ctx, cmd, heapBindings.indexPushOffset, blockBase.slot);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

template <typename LayoutT>
void PostProcessPass<LayoutT>::ExecuteHeap(const Context& ctx, VkCommandBuffer cmd, HeapBlockBase blockBase) const noexcept {
    ZHLN::Assert(cmd != VK_NULL_HANDLE, "{} requires a valid VkCommandBuffer.", "post-process fullscreen draw");
    ZHLN::Assert(Valid(), "Attempted to bind an invalid post-process pipeline.");
    ZHLN::Assert(heapBindings.indexPushOffset > 0, "Missing reflected descriptor-index offset.");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
    PushHeapIndex(ctx, cmd, heapBindings.indexPushOffset, blockBase.slot);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace ZHLN::Vk
