// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ComputePass.hpp
//
// VK_EXT_descriptor_heap compute pass wrappers. Skinning still has a
// leftover push-constant helper for its no-descriptor BDA path; bake /
// one-shot compute and every heap pass push through vkCmdPushDataEXT.

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <cassert>

namespace ZHLN::Vk {

namespace detail {

inline void DispatchThreads(
    VkCommandBuffer                cmd,
    const std::array<uint32_t, 3>& threadGroupSize,
    uint32_t                       threadCountX,
    uint32_t                       threadCountY,
    uint32_t                       threadCountZ
) noexcept {
    assert(threadGroupSize[0] > 0 && threadGroupSize[1] > 0 && threadGroupSize[2] > 0 && "Missing reflected compute thread-group size");
    ZHLN::Vk::Dispatch(cmd, threadCountX, threadCountY, threadCountZ, threadGroupSize[0], threadGroupSize[1], threadGroupSize[2]);
}

} // namespace detail

struct ComputePass {
    // Dispatch* uses a shader-reflected fixed domain. Dispatch*Threads accepts
    // a runtime logical domain. Only DispatchGroups accepts raw Vulkan groups.
    PipelineLayout          pipelineLayout; // Skinning only: legacy push-constant layout
    Pipeline                pipeline;
    std::vector<Pipeline>   pipelines; // Specialization variants share one mapping table
    std::array<uint32_t, 3> threadGroupSize {};
    std::array<uint32_t, 3> fixedDispatchSize {};
    uint32_t                heapIndexPushOffset = 0;

    /// Reflects Slang's `[numthreads]` and optional fixed dispatch metadata
    /// from the compiled compute entry point.
    [[nodiscard]] bool ReflectDispatchLayout(const ZHLN_ShaderDesc& shader) noexcept {
        auto reflected = ReflectComputeThreadGroupSize(shader);
        if (!reflected) {
            threadGroupSize   = {};
            fixedDispatchSize = {};
            return false;
        }
        threadGroupSize   = *reflected;
        fixedDispatchSize = ReflectComputeDispatchSize(shader).value_or(std::array<uint32_t, 3> {});
        return true;
    }

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

    /// Dispatches a logical thread domain. Workgroup counts are derived from
    /// the reflected Slang `[numthreads]`; callers never repeat local sizes.
    void DispatchThreads(VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ) const noexcept {
        detail::DispatchThreads(cmd, threadGroupSize, threadCountX, threadCountY, threadCountZ);
    }

    /// Dispatches the fixed logical domain declared by the Slang shader.
    void Dispatch(VkCommandBuffer cmd) const noexcept {
        assert(fixedDispatchSize[0] > 0 && fixedDispatchSize[1] > 0 && fixedDispatchSize[2] > 0 && "Shader does not declare a reflected fixed dispatch domain");
        detail::DispatchThreads(cmd, threadGroupSize, fixedDispatchSize[0], fixedDispatchSize[1], fixedDispatchSize[2]);
    }

    /// Escape hatch for algorithms that intentionally specify raw workgroup
    /// counts. Prefer Dispatch() for ordinary compute domains.
    static void DispatchGroups(VkCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept {
        ZHLN::Vk::DispatchGroups(cmd, groupCountX, groupCountY, groupCountZ);
    }

    // Dispatch with push data only (BDA/skinning-style compute).
    template <GpuTriviallyCopyable T>
    void DispatchThreads(VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ, const T& pushData) const noexcept {
        Bind(cmd);
        PushConstants(cmd, pushData);
        DispatchThreads(cmd, threadCountX, threadCountY, threadCountZ);
    }

    template <GpuTriviallyCopyable T>
    void Dispatch(VkCommandBuffer cmd, const T& pushData) const noexcept {
        Bind(cmd);
        PushConstants(cmd, pushData);
        Dispatch(cmd);
    }

    // VK_EXT_descriptor_heap dispatch: heaps are bound on the command buffer,
    // per-dispatch data via vkCmdPushDataEXT at offset 0.
    template <GpuTriviallyCopyable T>
    void DispatchHeapThreads(const Context& ctx, VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ, const T& pushData)
        const noexcept {
        Bind(cmd);
        PushData(ctx, cmd, 0, pushData);
        DispatchThreads(cmd, threadCountX, threadCountY, threadCountZ);
    }

    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd) const noexcept {
        (void) ctx;
        Bind(cmd);
        Dispatch(cmd);
    }

    template <GpuTriviallyCopyable T>
    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, const T& pushData) const noexcept {
        Bind(cmd);
        PushData(ctx, cmd, 0, pushData);
        Dispatch(cmd);
    }

    // Like DispatchHeap, but also pushes the descriptor-index word consumed by
    // HEAP_WITH_PUSH_INDEX mappings (frame parity / mip level / pass id).
    template <GpuTriviallyCopyable T>
    void DispatchHeapIndexedThreads(
        const Context&  ctx,
        VkCommandBuffer cmd,
        uint32_t        heapIndex,
        uint32_t        threadCountX,
        uint32_t        threadCountY,
        uint32_t        threadCountZ,
        const T&        pushData
    ) const noexcept {
        assert(heapIndexPushOffset > 0 && sizeof(T) <= heapIndexPushOffset && "Pass push struct overruns the reflected descriptor-index word");
        Bind(cmd);
        PushData(ctx, cmd, 0, pushData);
        PushHeapIndex(ctx, cmd, heapIndexPushOffset, heapIndex);
        DispatchThreads(cmd, threadCountX, threadCountY, threadCountZ);
    }

    void DispatchHeapIndexedThreads(
        const Context&  ctx,
        VkCommandBuffer cmd,
        uint32_t        heapIndex,
        uint32_t        threadCountX,
        uint32_t        threadCountY,
        uint32_t        threadCountZ
    ) const noexcept {
        assert(heapIndexPushOffset > 0 && "Missing reflected descriptor-index offset");
        Bind(cmd);
        PushHeapIndex(ctx, cmd, heapIndexPushOffset, heapIndex);
        DispatchThreads(cmd, threadCountX, threadCountY, threadCountZ);
    }

    /// Dispatches the fixed logical domain declared by the Slang shader.
    void DispatchHeapIndexed(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex) const noexcept {
        assert(fixedDispatchSize[0] > 0 && fixedDispatchSize[1] > 0 && fixedDispatchSize[2] > 0 && "Shader does not declare a reflected fixed dispatch domain");
        DispatchHeapIndexedThreads(ctx, cmd, heapIndex, fixedDispatchSize[0], fixedDispatchSize[1], fixedDispatchSize[2]);
    }

    template <GpuTriviallyCopyable T>
    void DispatchHeapIndexed(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, const T& pushData) const noexcept {
        assert(fixedDispatchSize[0] > 0 && fixedDispatchSize[1] > 0 && fixedDispatchSize[2] > 0 && "Shader does not declare a reflected fixed dispatch domain");
        DispatchHeapIndexedThreads(ctx, cmd, heapIndex, fixedDispatchSize[0], fixedDispatchSize[1], fixedDispatchSize[2], pushData);
    }
};

/// Heap-mode compute pass with a reflected set layout (LayoutT) driving its
/// binding table; frame-parity slot spans via the pushed index word.
template <typename LayoutT>
struct DoubleBufferedComputePass {
    // DispatchHeap uses a shader-reflected fixed domain; DispatchHeapThreads
    // accepts a runtime logical domain. LocalSize is always reflected.
    [[no_unique_address]] LayoutT layoutInstance {};
    Pipeline                      pipeline;
    HeapPassBindings              heapBindings;
    std::array<uint32_t, 3>       threadGroupSize {};
    std::array<uint32_t, 3>       fixedDispatchSize {};

    [[nodiscard]] bool BuildHeap(VkDevice device, HeapManager& heap, const ZHLN_ShaderDesc& shader, uint32_t indexPushOffset) noexcept {
        // Reflect the binding structure, [numthreads], and fixed logical domain,
        // then build a heap pipeline with a null layout + push data.
        auto reflectedGroupSize    = ReflectComputeThreadGroupSize(shader);
        auto reflectedDispatchSize = ReflectComputeDispatchSize(shader);
        if (!layoutInstance.Build(device, shader, VK_SHADER_STAGE_COMPUTE_BIT) || !reflectedGroupSize || !reflectedDispatchSize) {
            return false;
        }
        threadGroupSize   = *reflectedGroupSize;
        fixedDispatchSize = *reflectedDispatchSize;

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

    void DispatchHeapThreads(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ)
        const noexcept {
        assert(heapBindings.indexPushOffset > 0 && "Missing reflected descriptor-index offset");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
        PushHeapIndex(ctx, cmd, heapBindings.indexPushOffset, heapIndex);
        detail::DispatchThreads(cmd, threadGroupSize, threadCountX, threadCountY, threadCountZ);
    }

    template <GpuTriviallyCopyable T>
    void DispatchHeapThreads(
        const Context&  ctx,
        VkCommandBuffer cmd,
        uint32_t        heapIndex,
        uint32_t        threadCountX,
        uint32_t        threadCountY,
        uint32_t        threadCountZ,
        const T&        pushData
    ) const noexcept {
        assert(
            heapBindings.indexPushOffset > 0 && sizeof(T) <= heapBindings.indexPushOffset && "Pass push struct overruns the reflected descriptor-index word"
        );
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
        PushData(ctx, cmd, 0, pushData);
        PushHeapIndex(ctx, cmd, heapBindings.indexPushOffset, heapIndex);
        detail::DispatchThreads(cmd, threadGroupSize, threadCountX, threadCountY, threadCountZ);
    }

    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex) const noexcept {
        assert(fixedDispatchSize[0] > 0 && fixedDispatchSize[1] > 0 && fixedDispatchSize[2] > 0 && "Shader does not declare a reflected fixed dispatch domain");
        DispatchHeapThreads(ctx, cmd, heapIndex, fixedDispatchSize[0], fixedDispatchSize[1], fixedDispatchSize[2]);
    }

    template <GpuTriviallyCopyable T>
    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, const T& pushData) const noexcept {
        assert(fixedDispatchSize[0] > 0 && fixedDispatchSize[1] > 0 && fixedDispatchSize[2] > 0 && "Shader does not declare a reflected fixed dispatch domain");
        DispatchHeapThreads(ctx, cmd, heapIndex, fixedDispatchSize[0], fixedDispatchSize[1], fixedDispatchSize[2], pushData);
    }
};

} // namespace ZHLN::Vk
