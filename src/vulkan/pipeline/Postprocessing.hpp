// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/Postprocessing.hpp
//
// VK_EXT_descriptor_heap fullscreen-triangle pass. Layout authority lives in
// the compiled shader: LayoutT::Build reflects the set-0 binding structure,
// which the pass bakes into a PUSH_INDEX mapping table with the HeapManager.
// Each draw writes its descriptors into a fresh block and pushes that block's
// base slot, so a pass that draws several times per frame needs no per-draw
// slots reserved for it.

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

template <typename T>
concept PostProcessPushPayload = GpuTriviallyCopyable<T> && (sizeof(T) <= kScenePassPushPayloadBytes);

template <typename LayoutT>
struct PostProcessPass {
    [[no_unique_address]] LayoutT layoutInstance {};
    Pipeline                      pipeline;
    std::vector<Pipeline>         pipelines; // Specialization variants share one mapping table
    HeapPassBindings              heapBindings;

    /// Reflects the binding structure and bakes the mapping table itself.
    /// `lifecycle` decides which partition the pass's blocks are allocated from
    /// (see HeapLifecycle): the frame's, unless the caller records the pass
    /// outside the frame loop.
    [[nodiscard]] bool BuildHeap(
        VkDevice                        device,
        HeapManager&                    heap,
        const ShaderStages&             shaders,
        std::initializer_list<VkFormat> colorFormats,
        uint32_t                        indexPushOffset,
        HeapLifecycle                   lifecycle,
        bool                            additive = false,
        VkPipelineCache                 cache    = VK_NULL_HANDLE
    ) noexcept;

    [[nodiscard]] bool BuildHeapVariants(
        VkDevice                              device,
        HeapManager&                          heap,
        const ShaderStages&                   shaders,
        std::initializer_list<VkFormat>       colorFormats,
        std::span<const VkSpecializationInfo> specInfos,
        uint32_t                              indexPushOffset,
        HeapLifecycle                         lifecycle,
        bool                                  additive = false,
        VkPipelineCache                       cache    = VK_NULL_HANDLE
    ) noexcept;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return pipeline.Valid() || !pipelines.empty();
    }

    [[nodiscard]] auto HasHeapIndexPushOffset() const noexcept -> bool {
        return heapBindings.indexPushOffset > 0;
    }

    /// Writes the named descriptor values (Vk::Slot<"binding">(value)) into a
    /// fresh block from the frame's transient partition and returns its base,
    /// so the write and the draw cannot disagree about which descriptors the
    /// pass reads. Each name is matched against the shader's reflected binding
    /// names, so argument order carries no meaning; see
    /// HeapManager::WriteHeapParameters.
    template <typename... Slots>
    [[nodiscard]] auto WriteHeapParameters(const Context& ctx, HeapManager& heap, const Slots&... slots) const noexcept -> HeapBlockBase;

    /// `blockBase` is what WriteHeapParameters returned for this draw.
    template <PostProcessPushPayload T>
    void ExecuteHeap(const Context& ctx, VkCommandBuffer cmd, const T& pushData, HeapBlockBase blockBase) const noexcept;

    /// `variantIdx` selects the PIPELINE (RT/NoRT, SSR on/off); `blockBase`
    /// selects the descriptor block.
    template <PostProcessPushPayload T>
    void ExecuteVariantHeap(
        const Context& ctx, VkCommandBuffer cmd, uint32_t variantIdx, const T& pushData, HeapBlockBase blockBase
    ) const noexcept;

    void ExecuteHeap(const Context& ctx, VkCommandBuffer cmd, HeapBlockBase blockBase) const noexcept;
};

} // namespace ZHLN::Vk

#include "Postprocessing.inl"
