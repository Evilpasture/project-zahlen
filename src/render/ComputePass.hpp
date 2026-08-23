// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ComputePass.hpp
//
// VK_EXT_descriptor_heap compute pass wrappers. Skinning keeps a legacy
// push-constant path (pipeline layout + PushConstants) because it binds no
// descriptors; everything else dispatches through the heaps + push data.

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <cassert>

namespace ZHLN::Vk {

struct ComputePass {
    PipelineLayout        pipelineLayout; // Skinning only: legacy push-constant layout
    Pipeline              pipeline;
    std::vector<Pipeline> pipelines; // Specialization variants share one mapping table
    uint32_t              heapIndexPushOffset = 0;

    /// VK_EXT_descriptor_heap: null pipeline layout (spec-required) +
    /// set/binding -> heap mapping.
    [[nodiscard]] std::expected<void, ZHLN::Error> BuildHeap(
        VkDevice                                             device,
        const ZHLN_ShaderDesc&                               shader,
        const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
        uint32_t                                             indexPushOffset = 0
    ) noexcept;

    /// Heap-mode specialized variants (same mapping covers every variant).
    [[nodiscard]] std::expected<void, ZHLN::Error> BuildHeapVariants(
        VkDevice                                             device,
        const ZHLN_ShaderDesc&                               shader,
        std::span<const VkSpecializationInfo>                specInfos,
        const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
        uint32_t                                             indexPushOffset = 0
    ) noexcept;

    void Bind(VkCommandBuffer cmd) const noexcept {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
    }

    void BindVariant(VkCommandBuffer cmd, uint32_t variantIdx) const noexcept {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[variantIdx].Get());
    }

    // Skinning only: legacy push constants (no descriptors involved).
    template <GpuTriviallyCopyable T>
    void PushConstants(VkCommandBuffer cmd, const T& pushData) const noexcept {
        Push(cmd, pipelineLayout.Get(), VK_SHADER_STAGE_COMPUTE_BIT, pushData);
    }

    static void Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) noexcept {
        ZHLN_CmdDispatch(cmd, x, y, z);
    }

    // Dispatch with push data only (BDA/skinning-style compute).
    template <GpuTriviallyCopyable T>
    void Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept {
        Bind(cmd);
        PushConstants(cmd, pushData);
        Dispatch(cmd, x, y, z);
    }

    // VK_EXT_descriptor_heap dispatch: heaps are bound on the command buffer,
    // per-dispatch data via vkCmdPushDataEXT at offset 0.
    template <GpuTriviallyCopyable T>
    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept {
        Bind(cmd);
        PushData(ctx, cmd, 0, pushData);
        Dispatch(cmd, x, y, z);
    }

    // Like DispatchHeap, but also pushes the descriptor-index word consumed by
    // HEAP_WITH_PUSH_INDEX mappings (frame parity / mip level / pass id).
    template <GpuTriviallyCopyable T>
    void
        DispatchHeapIndexed(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept {
        assert(heapIndexPushOffset > 0 && sizeof(T) <= heapIndexPushOffset && "Pass push struct overruns the reflected descriptor-index word");
        Bind(cmd);
        PushData(ctx, cmd, 0, pushData);
        PushHeapIndex(ctx, cmd, heapIndexPushOffset, heapIndex);
        Dispatch(cmd, x, y, z);
    }

    void DispatchHeapIndexed(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, uint32_t x, uint32_t y, uint32_t z) const noexcept {
        assert(heapIndexPushOffset > 0 && "Missing reflected descriptor-index offset");
        Bind(cmd);
        PushHeapIndex(ctx, cmd, heapIndexPushOffset, heapIndex);
        Dispatch(cmd, x, y, z);
    }
};

/// Heap-mode compute pass with a reflected set layout (LayoutT) driving its
/// binding table; frame-parity slot spans via the pushed index word.
template <typename LayoutT>
struct DoubleBufferedComputePass {
    [[no_unique_address]] LayoutT layoutInstance {};
    Pipeline                      pipeline;
    HeapPassBindings              heapBindings;

    [[nodiscard]] bool BuildHeap(VkDevice device, HeapManager& heap, const ZHLN_ShaderDesc& shader, uint32_t indexPushOffset) noexcept {
        // Reflect the binding structure (drives the mapping table), then build
        // a heap pipeline with a null layout + push data.
        if (!layoutInstance.Build(device, shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
            return false;
        }

        BuildHeapPassBindings(heap, layoutInstance.reflectedSets[0], 0, indexPushOffset, 2, heapBindings);

        auto p_res = ComputePipelineBuilder().Shader(shader).Layout(VK_NULL_HANDLE).HeapMappings(heapBindings.GetInfo()).Build(device);
        if (!p_res) {
            return false;
        }
        pipeline = std::move(*p_res);
        return true;
    }

    template <typename... Args>
    void WriteHeap(const Context& ctx, HeapManager& heap, uint32_t heapIndex, Args&&... args) const noexcept {
        WriteHeapBindings(heap, ctx, heapBindings, heapIndex, std::forward<Args>(args)...);
    }

    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, uint32_t x, uint32_t y, uint32_t z) const noexcept {
        assert(heapBindings.indexPushOffset > 0 && "Missing reflected descriptor-index offset");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
        PushHeapIndex(ctx, cmd, heapBindings.indexPushOffset, heapIndex);
        vkCmdDispatch(cmd, x, y, z);
    }

    template <GpuTriviallyCopyable T>
    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept {
        assert(
            heapBindings.indexPushOffset > 0 && sizeof(T) <= heapBindings.indexPushOffset &&
            "Pass push struct overruns the reflected descriptor-index word"
        );
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
        PushData(ctx, cmd, 0, pushData);
        PushHeapIndex(ctx, cmd, heapBindings.indexPushOffset, heapIndex);
        vkCmdDispatch(cmd, x, y, z);
    }
};

} // namespace ZHLN::Vk
