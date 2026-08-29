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
    void DispatchHeapThreads(
        const Context&  ctx,
        VkCommandBuffer cmd,
        uint32_t        threadCountX,
        uint32_t        threadCountY,
        uint32_t        threadCountZ,
        const T&        pushData
    ) const noexcept {
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
        heap.WriteBindings(ctx, heapBindings, heapIndex, std::forward<Args>(args)...);
    }

    void DispatchHeapThreads(
        const Context&  ctx,
        VkCommandBuffer cmd,
        uint32_t        heapIndex,
        uint32_t        threadCountX,
        uint32_t        threadCountY,
        uint32_t        threadCountZ
    ) const noexcept {
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

/// Builds a standalone descriptor-heap compute pass from compiled SPIR-V.
/// Reflects `[numthreads]` and optional `Dispatch.SizeX/Y/Z` metadata.
[[nodiscard]] inline auto CreateHeapComputePass(VkDevice device, const ZHLN_ShaderDesc& shader) noexcept -> std::expected<ComputePass, Error> {
    if (shader.code == nullptr || shader.size == 0) {
        return std::unexpected(ShaderStageCreationError::ShaderLoadingFailed);
    }

    ComputePass pass;
    if (!pass.ReflectDispatchLayout(shader)) {
        return std::unexpected(SpirvLayoutError::ModuleParseFailed);
    }

    return ComputePipelineBuilder().Shader(shader).Layout(VK_NULL_HANDLE).HeapPipeline().Build(device).transform([&](Pipeline&& pipeline) {
        pass.pipeline = std::move(pipeline);
        return std::move(pass);
    });
}

/// Same as above, with a PUSH_INDEX mapping table (bake / pass slot spans).
[[nodiscard]] inline auto CreateHeapComputePass(
    VkDevice                                             device,
    const ZHLN_ShaderDesc&                               shader,
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
    uint32_t                                             indexPushOffset
) noexcept -> std::expected<ComputePass, Error> {
    if (shader.code == nullptr || shader.size == 0) {
        return std::unexpected(ShaderStageCreationError::ShaderLoadingFailed);
    }

    ComputePass pass;
    if (!pass.ReflectDispatchLayout(shader)) {
        return std::unexpected(SpirvLayoutError::ModuleParseFailed);
    }
    return pass.BuildHeap(device, shader, mapping, indexPushOffset).transform([&] { return std::move(pass); });
}

/// Builds and synchronously executes a heap compute shader on an immediate command ring.
template <GpuTriviallyCopyable PushT = std::monostate, QueueType QType = QueueType::Graphics, size_t Capacity = 8>
inline auto ExecuteImmediateCompute(
    const Context&                ctx,
    CommandRing<QType, Capacity>& ring,
    const ZHLN_ShaderDesc&        shader,
    uint32_t                      threadsX,
    uint32_t                      threadsY = 1,
    uint32_t                      threadsZ = 1,
    const PushT&                  pushData = {}
) noexcept -> std::expected<void, Error> {
    return CreateHeapComputePass(ctx.Device(), shader).transform([&](ComputePass pass) {
        ExecuteImmediate(ctx, ring, [&](VkCommandBuffer cmd) {
            pass.Bind(cmd);
            if constexpr (!std::is_same_v<PushT, std::monostate>) {
                PushData(ctx, cmd, 0, pushData);
            }
            pass.DispatchThreads(cmd, threadsX, threadsY, threadsZ);
            MemoryBarrier(
                cmd, {.src_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      .src_access = VK_ACCESS_2_SHADER_WRITE_BIT,
                      .dst_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      .dst_access = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT}
            );
        });
    });
}

/**
 * @brief Records a chain of dependent compute dispatches through one heap table.
 *
 * Two things every multi-dispatch pass was hand-rolling:
 *
 *  - Slot arithmetic. Heap descriptor writes are immediate host writes, so each
 *    in-frame step must bind and dispatch through its own slot or a later
 *    WriteBindings clobbers an earlier step's descriptors before the GPU reads
 *    them. Slots run frameIndex * slotSpan + step, matching the spans built at
 *    init time.
 *  - The compute->compute barrier between steps. The frame graph cannot supply
 *    it: it orders *passes* from their declared accesses, but a pass body is an
 *    opaque lambda, so dispatch-to-dispatch ordering inside a pass is invisible
 *    to it.
 *
 * `Step` takes the WriteBindings argument tail verbatim, because that order is
 * the shader's reflected binding order (see BuildHeapPassBindings), not anything
 * derivable from the pass's compile-time Usages list.
 *
 * A barrier is recorded after *every* step, including the last, preserving the
 * behaviour of the hand-written chains this replaces.
 */
class ComputeChain {
  public:
    constexpr ComputeChain(const Context& ctx, HeapManager& heap, VkCommandBuffer cmd, uint32_t frameIndex, uint32_t slotSpan) noexcept:
        _ctx(ctx), _heap(heap), _cmd(cmd), _frameIndex(frameIndex), _slotSpan(slotSpan) {
    }

    /// Bind, dispatch (sized from `extent`) and barrier one step of the chain.
    /// Pass `Barrier = false` for the final dispatch of a chain, which the
    /// hand-written code this replaces deliberately left unbarriered.
    template <bool Barrier = true, typename PushT, typename... Args>
    void Step(ComputePass& pass, const HeapPassBindings& bindings, VkExtent3D extent, const PushT& push, Args&&... args) noexcept {
        const uint32_t slot = _frameIndex * _slotSpan + _step++;
        _heap.WriteBindings(_ctx, bindings, slot, std::forward<Args>(args)...);
        pass.DispatchHeapIndexedThreads(_ctx, _cmd, slot, extent.width, extent.height, 1, push);
        if constexpr (Barrier) {
            ComputeChainBarrier(_cmd);
        }
    }

    [[nodiscard]] constexpr auto Slot() const noexcept -> uint32_t {
        return _frameIndex * _slotSpan + _step;
    }
    [[nodiscard]] constexpr auto StepCount() const noexcept -> uint32_t {
        return _step;
    }

  private:
    const Context&  _ctx;
    HeapManager&    _heap;
    VkCommandBuffer _cmd;
    uint32_t        _frameIndex;
    uint32_t        _slotSpan;
    uint32_t        _step = 0;
};

} // namespace ZHLN::Vk
