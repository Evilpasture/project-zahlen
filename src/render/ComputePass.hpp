// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ComputePass.hpp
#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

struct ComputePass {
    PipelineLayout        pipelineLayout;
    Pipeline              pipeline;
    std::vector<Pipeline> pipelines;

    [[nodiscard]] std::expected<void, ZHLN::Error> Build(
        VkDevice                   device,
        VkDescriptorSetLayout      descriptorLayout,
        const ZHLN_ShaderDesc&     shader,
        const VkPushConstantRange* pushConstants = nullptr,
        uint32_t                   pushCount     = 0
    ) noexcept;

    /// VK_EXT_descriptor_heap variant: null pipeline layout (spec-required) +
    /// set/binding -> heap mapping. Dispatch through DispatchHeap (vkCmdPushDataEXT).
    [[nodiscard]] std::expected<void, ZHLN::Error> BuildHeap(
        VkDevice device, const ZHLN_ShaderDesc& shader, const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping
    ) noexcept;

    /// Heap-mode specialized variants (same mapping covers every variant).
    [[nodiscard]] std::expected<void, ZHLN::Error> BuildHeapVariants(
        VkDevice device, const ZHLN_ShaderDesc& shader, std::span<const VkSpecializationInfo> specInfos,
        const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping
    ) noexcept;

    [[nodiscard]] std::expected<void, ZHLN::Error> BuildVariants(
        VkDevice                              device,
        VkDescriptorSetLayout                 descriptorLayout,
        const ZHLN_ShaderDesc&                shader,
        std::span<const VkSpecializationInfo> specInfos,
        const VkPushConstantRange*            pushConstants = nullptr,
        uint32_t                              pushCount     = 0
    ) noexcept;

    // --- Stateful Bind and Push Helpers ---

    void Bind(VkCommandBuffer cmd) const noexcept {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
    }

    void BindSet(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t firstSet = 0) const noexcept {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout.Get(), firstSet, 1, &set, 0, nullptr);
    }

    void BindVariant(VkCommandBuffer cmd, uint32_t variantIdx) const noexcept {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[variantIdx].Get());
    }

    template <GpuTriviallyCopyable T>
    void DispatchVariant(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t variantIdx, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept {
        BindVariant(cmd, variantIdx);
        BindSet(cmd, set);
        PushConstants(cmd, pushData);
        Dispatch(cmd, x, y, z);
    }

    template <GpuTriviallyCopyable T>
    void PushConstants(VkCommandBuffer cmd, const T& pushData) const noexcept {
        Push(cmd, pipelineLayout.Get(), VK_SHADER_STAGE_COMPUTE_BIT, pushData);
    }

    // VK_EXT_descriptor_heap dispatch: no descriptor set (heaps are bound on
    // the command buffer), per-dispatch data via vkCmdPushDataEXT at offset 0.
    template <GpuTriviallyCopyable T>
    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept {
        Bind(cmd);
        PushData(ctx, cmd, 0, pushData);
        Dispatch(cmd, x, y, z);
    }

    // Like DispatchHeap, but also pushes the descriptor-index word consumed by
    // HEAP_WITH_PUSH_INDEX mappings (frame parity / mip level / pass id).
    template <GpuTriviallyCopyable T>
    void DispatchHeapIndexed(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, uint32_t x, uint32_t y, uint32_t z, const T& pushData)
        const noexcept {
        static_assert(sizeof(T) <= kHeapIndexPushOffset, "Pass push struct overruns the descriptor-index word");
        Bind(cmd);
        PushData(ctx, cmd, 0, pushData);
        PushHeapIndex(ctx, cmd, kHeapIndexPushOffset, heapIndex);
        Dispatch(cmd, x, y, z);
    }

    void DispatchHeapIndexed(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, uint32_t x, uint32_t y, uint32_t z) const noexcept {
        Bind(cmd);
        PushHeapIndex(ctx, cmd, kHeapIndexPushOffset, heapIndex);
        Dispatch(cmd, x, y, z);
    }

    static void Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) noexcept {
        ZHLN_CmdDispatch(cmd, x, y, z);
    }

    // --- High-Level Convenience Dispatches ---

    // Dispatch with no Descriptor Set (e.g. BDA only, like Skinning)
    template <GpuTriviallyCopyable T>
    void Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept {
        Bind(cmd);
        PushConstants(cmd, pushData);
        Dispatch(cmd, x, y, z);
    }

    // Dispatch with Descriptor Set, but no Push Constants (like Clustering)
    void Dispatch(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t x, uint32_t y, uint32_t z) const noexcept {
        Bind(cmd);
        BindSet(cmd, set);
        Dispatch(cmd, x, y, z);
    }

    // Dispatch with both Descriptor Set & Push Constants
    template <GpuTriviallyCopyable T>
    void Dispatch(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept {
        Bind(cmd);
        BindSet(cmd, set);
        PushConstants(cmd, pushData);
        Dispatch(cmd, x, y, z);
    }
};

template <typename LayoutT>
struct DoubleBufferedComputePass {
    [[no_unique_address]] LayoutT         layoutInstance {};
    DescriptorPool                        pool;
    ZHLN::DoubleBuffered<VkDescriptorSet> sets;
    PipelineLayout                        pipelineLayout;
    Pipeline                              pipeline;

    // VK_EXT_descriptor_heap mode (BuildHeap): binding mapping + slots.
    HeapPassBindings heapBindings;

    [[nodiscard]] bool
        Build(VkDevice device, const ZHLN_ShaderDesc& shader, const VkPushConstantRange* pushConstants = nullptr, uint32_t pushCount = 0) noexcept {
        // Layout authority lives in the compiled shader: reflect the set layout.
        if (!layoutInstance.Build(device, shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
            return false;
        }
        pool    = layoutInstance.CreatePool(device, 2);
        sets[0] = layoutInstance.Allocate(device, pool.Get(), layoutInstance.GetSetLayout());
        sets[1] = layoutInstance.Allocate(device, pool.Get(), layoutInstance.GetSetLayout());

        PipelineLayoutBuilder builder(device);
        builder.AddDescriptorSetLayout(layoutInstance.GetSetLayout());
        for (uint32_t i = 0; i < pushCount; ++i) {
            builder.AddPushConstant(pushConstants[i].stageFlags, pushConstants[i].size, pushConstants[i].offset);
        }

        auto layout_res = builder.Build();
        if (!layout_res) {
            return false;
        }
        pipelineLayout = std::move(layout_res.value());

        auto p_res = ComputePipelineBuilder().Shader(shader).Layout(pipelineLayout.Get()).Build(device);
        if (!p_res) {
            return false;
        }
        pipeline = std::move(*p_res);
        return true;
    }

    template <typename... Args>
    void WriteNext(VkDevice device, Args&&... args) const noexcept {
        layoutInstance.Write(device, sets.Next(), std::forward<Args>(args)...);
    }

    void Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) const noexcept {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
        VkDescriptorSet set = sets.Next();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout.Get(), 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, x, y, z);
    }

    template <GpuTriviallyCopyable T>
    void Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
        VkDescriptorSet set = sets.Next();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout.Get(), 0, 1, &set, 0, nullptr);
        Push(cmd, pipelineLayout.Get(), VK_SHADER_STAGE_COMPUTE_BIT, pushData);
        vkCmdDispatch(cmd, x, y, z);
    }

    void Flip() noexcept {
        sets.Flip();
    }

    // ============================================================================
    // VK_EXT_descriptor_heap mode
    // ============================================================================

    [[nodiscard]] bool BuildHeap(VkDevice device, HeapManager& heap, const ZHLN_ShaderDesc& shader) noexcept {
        // Reflect the set layout (drives the mapping table), then build a heap
        // pipeline with a null layout + push data.
        if (!layoutInstance.Build(device, shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
            return false;
        }

        BuildHeapPassBindings(heap, layoutInstance.reflectedSets[0], 0, kHeapIndexPushOffset, 2, heapBindings);

        auto p_res = ComputePipelineBuilder().Shader(shader).Layout(VK_NULL_HANDLE).HeapMappings(heapBindings.GetInfo()).Build(device);
        if (!p_res) {
            return false;
        }
        pipeline = std::move(*p_res);
        pipelineLayout = {};
        return true;
    }

    template <typename... Args>
    void WriteHeap(const Context& ctx, HeapManager& heap, uint32_t heapIndex, Args&&... args) const noexcept {
        WriteHeapBindings(heap, ctx, heapBindings, heapIndex, std::forward<Args>(args)...);
    }

    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, uint32_t x, uint32_t y, uint32_t z) const noexcept {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
        PushHeapIndex(ctx, cmd, kHeapIndexPushOffset, heapIndex);
        vkCmdDispatch(cmd, x, y, z);
    }

    template <GpuTriviallyCopyable T>
    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
        PushData(ctx, cmd, 0, pushData);
        PushHeapIndex(ctx, cmd, kHeapIndexPushOffset, heapIndex);
        vkCmdDispatch(cmd, x, y, z);
    }
};

} // namespace ZHLN::Vk
