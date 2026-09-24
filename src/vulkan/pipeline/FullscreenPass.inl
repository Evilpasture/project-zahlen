// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/FullscreenPass.inl
#pragma once

#include "FullscreenPass.hpp"

namespace ZHLN::Vk {

template <typename LayoutT>
std::expected<void, ZHLN::ErrorCode> FullscreenPass<LayoutT>::BuildHeap(
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
        return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
    }

    if (auto built = BuildHeapPassBindings(heap, layoutInstance.sets[0], 0, indexPushOffset, lifecycle, heapBindings); !built) {
        return std::unexpected(built.error());
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
        return std::unexpected(p_res.error());
    }
    pipeline = std::move(*p_res);
    return {};
}

template <typename LayoutT>
std::expected<void, ZHLN::ErrorCode> FullscreenPass<LayoutT>::BuildHeapVariants(
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
        return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
    }

    if (auto built = BuildHeapPassBindings(heap, layoutInstance.sets[0], 0, indexPushOffset, lifecycle, heapBindings); !built) {
        return std::unexpected(built.error());
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
            return std::unexpected(p_res.error());
        }
        pipelines.push_back(std::move(*p_res));
    }

    if (pipelines.empty()) {
        return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
    }
    return {};
}

template <typename LayoutT>
template <typename Declared, typename... Slots>
auto FullscreenPass<LayoutT>::WriteHeapParameters(const Context& ctx, HeapManager& heap, const Slots&... slots) const noexcept -> HeapBlockBase {
    return heap.template WriteHeapParameters<Declared>(ctx, heapBindings, slots...);
}

template <typename LayoutT>
template <ShaderProgram... Modules, FullscreenPushPayload T>
void FullscreenPass<LayoutT>::ExecuteHeap(const Context& ctx, VkCommandBuffer cmd, const T& pushData, HeapBlockBase blockBase) const noexcept {
    static_assert(sizeof...(Modules) > 0, "name the shader module(s) this draw is recorded for: ExecuteHeap<Shaders::Modules::X>(...)");
    static_assert(
        Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
        "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
    );
    ZHLN::Assert(cmd != VK_NULL_HANDLE, "{} requires a valid VkCommandBuffer.", "fullscreen draw");
    ZHLN::Assert(Valid(), "Attempted to bind an invalid fullscreen pipeline.");
    ZHLN::Assert(heapBindings.indexPushOffset > 0, "Missing reflected descriptor-index offset.");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
    PushData(cmd, 0, pushData);
    // The mapping is slot-independent: what travels here is the block's base
    // slot, not an ordinal.
    PushHeapIndex(cmd, heapBindings.indexPushOffset, blockBase.slot);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

template <typename LayoutT>
template <ShaderProgram... Modules, FullscreenPushPayload T>
void FullscreenPass<LayoutT>::ExecuteVariantHeap(
    const Context&  ctx,
    VkCommandBuffer cmd,
    uint32_t        variantIdx,
    const T&        pushData,
    HeapBlockBase   blockBase
) const noexcept {
    static_assert(sizeof...(Modules) > 0, "name the shader modules this draw can run: ExecuteVariantHeap<Shaders::Modules::X, ...>(...)");
    static_assert(
        Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
        "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
    );
    ZHLN::Assert(cmd != VK_NULL_HANDLE, "{} requires a valid VkCommandBuffer.", "fullscreen draw");
    ZHLN::Assert(heapBindings.indexPushOffset > 0, "Missing reflected descriptor-index offset.");
    ZHLN::Assert(variantIdx < pipelines.size(), "Fullscreen pipeline variant index {} is out of bounds ({} variants).", variantIdx, pipelines.size());
    ZHLN::Assert(pipelines[variantIdx].Valid(), "Attempted to bind an invalid fullscreen pipeline variant {}.", variantIdx);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[variantIdx].Get());
    PushData(cmd, 0, pushData);
    // The mapping is slot-independent: what travels here is the block's base
    // slot, not an ordinal.
    PushHeapIndex(cmd, heapBindings.indexPushOffset, blockBase.slot);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

template <typename LayoutT>
void FullscreenPass<LayoutT>::ExecuteHeap(const Context& ctx, VkCommandBuffer cmd, HeapBlockBase blockBase) const noexcept {
    ZHLN::Assert(cmd != VK_NULL_HANDLE, "{} requires a valid VkCommandBuffer.", "fullscreen draw");
    ZHLN::Assert(Valid(), "Attempted to bind an invalid fullscreen pipeline.");
    ZHLN::Assert(heapBindings.indexPushOffset > 0, "Missing reflected descriptor-index offset.");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
    PushHeapIndex(cmd, heapBindings.indexPushOffset, blockBase.slot);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace ZHLN::Vk
