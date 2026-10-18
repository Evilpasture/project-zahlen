// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "FullscreenPass.hpp"

namespace ZHLN::Vk {

template <typename LayoutT>
std::expected<void, ErrorCode> FullscreenPass<LayoutT>::BuildHeap(
    VkDevice                        device,
    HeapManager&                    heap,
    ShaderStagesView             shaders,
    std::initializer_list<VkFormat> colorFormats,
    uint32_t                        indexPushOffset,
    HeapLifecycle                   lifecycle,
    bool                            additive,
    PipelineBinaryStore*              binaries,
    const HeapMappingBundle*        extraMappings
) noexcept {

    if (!layoutInstance.Build(device, shaders)) {
        return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
    }

    if (auto built = BuildHeapPassBindings(heap, layoutInstance.sets[0], 0, indexPushOffset, lifecycle, heapBindings); !built) {
        return std::unexpected(built.error());
    }

    // The pass's set-0 mappings, plus any shared bundle the module's other
    // sets need (Set-1 scene block). Extras for undeclared variables are
    // legal; missing ones are not (VUID-...-11312).
    const HeapMappingBundle merged = HeapMappingBundle::Merge(heapBindings, extraMappings);
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* mappings = merged.GetInfo();

    auto builder = PipelineBuilder {}
                       .Shaders(shaders)
                       .Binaries(binaries)
                       .HeapMappings(mappings, mappings)
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
std::expected<void, ErrorCode> FullscreenPass<LayoutT>::BuildHeapVariants(
    VkDevice                              device,
    HeapManager&                          heap,
    ShaderStagesView                   shaders,
    std::initializer_list<VkFormat>       colorFormats,
    std::span<const VkSpecializationInfo> specInfos,
    uint32_t                              indexPushOffset,
    HeapLifecycle                         lifecycle,
    bool                                  additive,
    PipelineBinaryStore*                    binaries,
    const HeapMappingBundle*              extraMappings
) noexcept {

    if (!layoutInstance.Build(device, shaders)) {
        return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
    }

    if (auto built = BuildHeapPassBindings(heap, layoutInstance.sets[0], 0, indexPushOffset, lifecycle, heapBindings); !built) {
        return std::unexpected(built.error());
    }

    // Same set-0 + shared-bundle merge as BuildHeap above.
    const HeapMappingBundle merged = HeapMappingBundle::Merge(heapBindings, extraMappings);
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* mappings = merged.GetInfo();

    pipelines.clear();
    pipelines.reserve(specInfos.size());

    for (const auto& spec: specInfos) {
        auto builder = PipelineBuilder {}
                           .Shaders(shaders)
                           .Binaries(binaries)
                           .HeapMappings(mappings, mappings)
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
auto FullscreenPass<LayoutT>::WriteHeapParameters(HeapManager& heap, const Slots&... slots) const noexcept -> HeapBlockBase {
    return heap.template WriteHeapParameters<Declared>(heapBindings, slots...);
}

template <typename LayoutT>
template <ShaderProgram... Modules, FullscreenPushPayload T>
void FullscreenPass<LayoutT>::ExecuteHeap(RasterPassContextBase& ctx, ActivePipeline active, BoundHeapPass heap, const T& pushData) const noexcept {
    static_assert(sizeof...(Modules) > 0, "name the shader module(s) this draw is recorded for: ExecuteHeap<Shaders::Modules::X>(...)");
    static_assert(
        Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
        "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
    );
    vkCmdBindPipeline(ctx.Cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, active);
    PushData(ctx.Cmd(), 0, pushData);
    heap.Push(ctx.Cmd());
    vkCmdDraw(ctx.Cmd(), 3, 1, 0, 0);
}

template <typename LayoutT>
void FullscreenPass<LayoutT>::ExecuteHeap(RasterPassContextBase& ctx, ActivePipeline active, BoundHeapPass heap) const noexcept {
    vkCmdBindPipeline(ctx.Cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, active);
    heap.Push(ctx.Cmd());
    vkCmdDraw(ctx.Cmd(), 3, 1, 0, 0);
}

}
