// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "ActivePipeline.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Vk {

template <typename T>
concept FullscreenPushPayload = GpuTriviallyCopyable<T>;

template <typename LayoutT>
struct FullscreenPass {
    [[no_unique_address]] LayoutT layoutInstance {};
    Pipeline                      pipeline;
    std::vector<Pipeline>         pipelines;
    HeapPassBindings              heapBindings;

    [[nodiscard]] std::expected<void, ErrorCode> BuildHeap(
        VkDevice                        device,
        HeapManager&                    heap,
        ShaderStagesView             shaders,
        std::initializer_list<VkFormat> colorFormats,
        uint32_t                        indexPushOffset,
        HeapLifecycle                   lifecycle,
        bool                            additive = false,
        PipelineBinaryStore*              binaries = nullptr,
        // Extra mappings merged into both stages' pMappings (e.g. the scene
        // block's Set-1 bindings when the module references `scene`): every
        // shader variable needs a mapping under
        // VUID-VkGraphicsPipelineCreateInfo-flags-11312, and extras for
        // variables a variant does not declare are legal (the decal pass has
        // carried one from day one).
        const HeapMappingBundle*      extraMappings = nullptr
    ) noexcept;

    [[nodiscard]] std::expected<void, ErrorCode> BuildHeapVariants(
        VkDevice                              device,
        HeapManager&                          heap,
        ShaderStagesView                   shaders,
        std::initializer_list<VkFormat>       colorFormats,
        std::span<const VkSpecializationInfo> specInfos,
        uint32_t                              indexPushOffset,
        HeapLifecycle                         lifecycle,
        bool                                  additive = false,
        PipelineBinaryStore*                    binaries = nullptr,
        const HeapMappingBundle*            extraMappings = nullptr
    ) noexcept;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return pipeline.Valid() || !pipelines.empty();
    }

    [[nodiscard]] auto HasHeapIndexPushOffset() const noexcept -> bool {
        return heapBindings.indexPushOffset > 0;
    }

    template <typename Declared, typename... Slots>
    [[nodiscard]] auto WriteHeapParameters(HeapManager& heap, const Slots&... slots) const noexcept -> HeapBlockBase;

    // Recorded draws take the proofs, not the raw state: `ctx` carries the
    // command buffer (references cannot be null), `active` a built pipeline,
    // `heap` a non-zero heap-index push. The three asserts this used to carry
    // are unrepresentable here -- record-time factories (ActivePipeline::From,
    // BoundHeapPass::Create) refuse to produce anything the draw cannot use.
    template <ShaderProgram... Modules, FullscreenPushPayload T>
    void ExecuteHeap(RasterPassContextBase& ctx, ActivePipeline active, BoundHeapPass heap, const T& pushData) const noexcept;

    void ExecuteHeap(RasterPassContextBase& ctx, ActivePipeline active, BoundHeapPass heap) const noexcept;
};

}

#include "FullscreenPass.inl"
