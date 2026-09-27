// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

template <typename T>
concept FullscreenPushPayload = GpuTriviallyCopyable<T>;

template <typename LayoutT>
struct FullscreenPass {
    [[no_unique_address]] LayoutT layoutInstance {};
    Pipeline                      pipeline;
    std::vector<Pipeline>         pipelines;
    HeapPassBindings              heapBindings;

    [[nodiscard]] std::expected<void, ZHLN::ErrorCode> BuildHeap(
        VkDevice                        device,
        HeapManager&                    heap,
        const ShaderStages&             shaders,
        std::initializer_list<VkFormat> colorFormats,
        uint32_t                        indexPushOffset,
        HeapLifecycle                   lifecycle,
        bool                            additive = false,
        VkPipelineCache                 cache    = VK_NULL_HANDLE
    ) noexcept;

    [[nodiscard]] std::expected<void, ZHLN::ErrorCode> BuildHeapVariants(
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

    template <typename Declared, typename... Slots>
    [[nodiscard]] auto WriteHeapParameters(const Context& ctx, HeapManager& heap, const Slots&... slots) const noexcept -> HeapBlockBase;

    template <ShaderProgram... Modules, FullscreenPushPayload T>
    void ExecuteHeap(const Context& ctx, VkCommandBuffer cmd, const T& pushData, HeapBlockBase blockBase) const noexcept;

    template <ShaderProgram... Modules, FullscreenPushPayload T>
    void ExecuteVariantHeap(
        const Context& ctx, VkCommandBuffer cmd, uint32_t variantIdx, const T& pushData, HeapBlockBase blockBase
    ) const noexcept;

    void ExecuteHeap(const Context& ctx, VkCommandBuffer cmd, HeapBlockBase blockBase) const noexcept;
};

}

#include "FullscreenPass.inl"
