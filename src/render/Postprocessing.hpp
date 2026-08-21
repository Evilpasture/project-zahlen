// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Postprocessing.hpp
#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

template <typename LayoutT>
struct PostProcessPass {
    [[no_unique_address]] LayoutT         layoutInstance {};
    DescriptorPool                        pool;
    ZHLN::DoubleBuffered<VkDescriptorSet> sets;
    PipelineLayout                        pipelineLayout;
    Pipeline                              pipeline;
    std::vector<Pipeline>                 pipelines; // Unified storage for variants

    // VK_EXT_descriptor_heap: binding mapping table + slot layout (valid when
    // built through BuildHeap/BuildHeapVariants).
    HeapPassBindings heapBindings;

    [[nodiscard]] bool Build(
        VkDevice                        device,
        const ShaderStages&             shaders,
        std::initializer_list<VkFormat> colorFormats,
        const VkPushConstantRange*      pushConstants = nullptr,
        uint32_t                        pushCount     = 0,
        bool                            additive      = false
    ) noexcept;

    [[nodiscard]] bool BuildVariants(
        VkDevice                              device,
        const ShaderStages&                   shaders,
        std::initializer_list<VkFormat>       colorFormats,
        const VkPushConstantRange*            pushConstants,
        uint32_t                              pushCount,
        std::span<const VkSpecializationInfo> specInfos,
        bool                                  additive = false
    ) noexcept;

    template <typename TargetT, typename PushT, typename... Args>
    auto ExecuteWithTransitions(VkCommandBuffer cmd, VkDevice device, TargetT& targetRenderTarget, const PushT& pc, Args&&... inputs)
        -> TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>;

    template <typename TargetT, typename PushT, typename... Args>
    auto
        ExecuteVariantWithTransitions(VkCommandBuffer cmd, VkDevice device, TargetT& targetRenderTarget, uint32_t variantIdx, const PushT& pc, Args&&... inputs)
            -> TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>;

    template <typename... Args>
    void WriteNext(VkDevice device, Args&&... args) const noexcept;

    template <typename... Args>
    void WriteIndex(VkDevice device, uint32_t idx, Args&&... args) noexcept {
        layoutInstance.Write(device, sets[idx], std::forward<Args>(args)...);
    }

    template <GpuTriviallyCopyable T>
    void Execute(VkCommandBuffer cmd, const T& pushData, VkShaderStageFlags stages = VK_SHADER_STAGE_FRAGMENT_BIT) const noexcept;

    template <GpuTriviallyCopyable T>
    void ExecuteVariant(VkCommandBuffer cmd, uint32_t variantIdx, const T& pushData, VkShaderStageFlags stages = VK_SHADER_STAGE_FRAGMENT_BIT) const noexcept;

    void Execute(VkCommandBuffer cmd) const noexcept;

    // ============================================================================
    // VK_EXT_descriptor_heap mode: no descriptor sets/pools/layouts. The pass
    // reflects the set layout (for binding structure), bakes a PUSH_INDEX
    // mapping table, and draws with a null pipeline layout + push data.
    // ============================================================================

    [[nodiscard]] bool BuildHeap(
        VkDevice device, HeapManager& heap, const ShaderStages& shaders, std::initializer_list<VkFormat> colorFormats, bool additive = false
    ) noexcept;

    [[nodiscard]] bool BuildHeapVariants(
        VkDevice                              device,
        HeapManager&                          heap,
        const ShaderStages&                   shaders,
        std::initializer_list<VkFormat>       colorFormats,
        std::span<const VkSpecializationInfo> specInfos,
        bool                                  additive = false
    ) noexcept;

    template <typename... Args>
    void WriteHeap(const Context& ctx, HeapManager& heap, uint32_t heapIndex, Args&&... args) const noexcept;

    template <GpuTriviallyCopyable T>
    void ExecuteHeap(const Context& ctx, VkCommandBuffer cmd, const T& pushData, uint32_t heapIndex, VkShaderStageFlags stages = VK_SHADER_STAGE_FRAGMENT_BIT)
        const noexcept;

    template <GpuTriviallyCopyable T>
    void ExecuteVariantHeap(
        const Context& ctx, VkCommandBuffer cmd, uint32_t variantIdx, const T& pushData, uint32_t heapIndex,
        VkShaderStageFlags stages = VK_SHADER_STAGE_FRAGMENT_BIT
    ) const noexcept;

    void ExecuteHeap(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex) const noexcept;

    void Flip() noexcept {
        sets.Flip();
    }
};

} // namespace ZHLN::Vk

#include "Postprocessing.inl"
