// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Postprocessing.hpp
//
// VK_EXT_descriptor_heap fullscreen-triangle pass. Layout authority lives in
// the compiled shader: LayoutT::Build reflects the set-0 binding structure,
// which the pass bakes into a PUSH_INDEX mapping table (frame-parity slot
// spans) with the HeapManager.

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

template <typename T>
concept PostProcessPushPayload = GpuTriviallyCopyable<T> && (sizeof(T) <= sizeof(::ZHLN::ScenePassPushConstants));

template <typename LayoutT>
struct PostProcessPass {
    [[no_unique_address]] LayoutT layoutInstance {};
    Pipeline                      pipeline;
    std::vector<Pipeline>         pipelines; // Specialization variants share one mapping table
    HeapPassBindings              heapBindings;

    [[nodiscard]] bool BuildHeap(
        VkDevice                        device,
        HeapManager&                    heap,
        const ShaderStages&             shaders,
        std::initializer_list<VkFormat> colorFormats,
        uint32_t                        indexPushOffset,
        bool                            additive = false
    ) noexcept;

    [[nodiscard]] bool BuildHeapVariants(
        VkDevice                              device,
        HeapManager&                          heap,
        const ShaderStages&                   shaders,
        std::initializer_list<VkFormat>       colorFormats,
        std::span<const VkSpecializationInfo> specInfos,
        uint32_t                              indexPushOffset,
        bool                                  additive = false
    ) noexcept;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return pipeline.Valid() || !pipelines.empty();
    }

    [[nodiscard]] auto HasHeapIndexPushOffset() const noexcept -> bool {
        return heapBindings.indexPushOffset > 0;
    }

    /// Writes the pass descriptors into the slot span selected by `heapIndex`
    /// (frame parity). Argument order mirrors the shader's set-0 declaration
    /// order; sampler positions are skipped.
    template <typename... Args>
    void WriteHeap(const Context& ctx, HeapManager& heap, uint32_t heapIndex, Args&&... args) const noexcept;

    template <PostProcessPushPayload T>
    void ExecuteHeap(const Context& ctx, VkCommandBuffer cmd, const T& pushData, uint32_t heapIndex) const noexcept;

    template <PostProcessPushPayload T>
    void ExecuteVariantHeap(const Context& ctx, VkCommandBuffer cmd, uint32_t variantIdx, const T& pushData, uint32_t heapIndex) const noexcept;

    void ExecuteHeap(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex) const noexcept;
};

} // namespace ZHLN::Vk

#include "Postprocessing.inl"
