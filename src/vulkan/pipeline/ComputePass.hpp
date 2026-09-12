// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/ComputePass.hpp
//
// VK_EXT_descriptor_heap compute pass wrappers. Skinning still has a
// leftover push-constant helper for its no-descriptor BDA path; bake /
// one-shot compute and every heap pass push through vkCmdPushDataEXT.

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Log.hpp>

namespace ZHLN::Vk {

enum class ComputeDomain : uint8_t { Dynamic, Fixed };

template <typename T>
concept HeapPassPushPayload = GpuTriviallyCopyable<T> && (sizeof(T) <= kScenePassPushPayloadBytes);

namespace TemplatedDetail {

[[nodiscard]] inline constexpr auto HasPositiveExtent(const std::array<uint32_t, 3>& extent) noexcept -> bool {
    return extent[0] > 0 && extent[1] > 0 && extent[2] > 0;
}

/// Bind (optional), push (optional), then one vkCmdDispatch via Vk::Dispatch.
/// Heap index 0-offset means "do not push an index word".
struct ComputeDispatchDesc {
    VkCommandBuffer         cmd {};
    std::array<uint32_t, 3> threadGroupSize {};
    uint32_t                threadCountX    = 0;
    uint32_t                threadCountY    = 0;
    uint32_t                threadCountZ    = 0;
    VkPipeline              pipeline        = VK_NULL_HANDLE;
    bool                    bind            = false;
    const Context*          ctx             = nullptr;
    VkPipelineLayout        legacyLayout    = VK_NULL_HANDLE;
    uint32_t                heapIndexOffset = 0;
    uint32_t                heapIndex       = 0;
};

template <typename PushT = std::monostate>
inline void RecordComputeDispatch(const ComputeDispatchDesc& desc, const PushT* pushData = nullptr) noexcept {
    ZHLN::Assert(desc.cmd != VK_NULL_HANDLE);
    ZHLN::Assert(HasPositiveExtent(desc.threadGroupSize));
    ZHLN::Assert(desc.threadCountX > 0 && desc.threadCountY > 0 && desc.threadCountZ > 0);
    if (desc.bind) {
        ZHLN::Assert(desc.pipeline != VK_NULL_HANDLE);
        vkCmdBindPipeline(desc.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, desc.pipeline);
    }
    if constexpr (!std::is_same_v<PushT, std::monostate>) {
        ZHLN::Assert(pushData != nullptr);
        if (desc.legacyLayout != VK_NULL_HANDLE) {
            Push(desc.cmd, desc.legacyLayout, VK_SHADER_STAGE_COMPUTE_BIT, *pushData);
        } else {
            ZHLN::Assert(desc.ctx != nullptr);
            PushData(*desc.ctx, desc.cmd, 0, *pushData);
        }
    }
    if (desc.heapIndexOffset > 0) {
        ZHLN::Assert(desc.ctx != nullptr);
        PushHeapIndex(*desc.ctx, desc.cmd, desc.heapIndexOffset, desc.heapIndex);
    }
    Dispatch(desc.cmd, desc.threadCountX, desc.threadCountY, desc.threadCountZ, desc.threadGroupSize[0], desc.threadGroupSize[1], desc.threadGroupSize[2]);
}

} // namespace TemplatedDetail

template <ComputeDomain Domain = ComputeDomain::Dynamic>
struct ComputePass {
    PipelineLayout          pipelineLayout; // Skinning only: legacy push-constant layout
    Pipeline                pipeline;
    std::vector<Pipeline>   pipelines; // Specialization variants share one mapping table
    std::array<uint32_t, 3> threadGroupSize {};
    std::array<uint32_t, 3> fixedDispatchSize {};
    uint32_t                heapIndexPushOffset = 0;

    /// Reflects Slang's `[numthreads]` and optional fixed dispatch metadata
    /// from the compiled compute entry point. Fixed-domain passes require the
    /// shader to publish Dispatch.SizeX/Y/Z; dynamic passes only require
    /// `[numthreads]`.
    [[nodiscard]] bool ReflectDispatchLayout(const ZHLN_ShaderDesc& shader) noexcept {
        auto reflected = ReflectComputeThreadGroupSize(shader);
        if (!reflected) {
            threadGroupSize   = {};
            fixedDispatchSize = {};
            return false;
        }

        threadGroupSize = *reflected;

        auto reflectedFixed = ReflectComputeDispatchSize(shader);
        if constexpr (Domain == ComputeDomain::Fixed) {
            if (!reflectedFixed) {
                fixedDispatchSize = {};
                return false;
            }
            fixedDispatchSize = *reflectedFixed;
        } else {
            fixedDispatchSize = reflectedFixed.value_or(std::array<uint32_t, 3> {});
        }
        ZHLN::Assert(TemplatedDetail::HasPositiveExtent(threadGroupSize));
        if constexpr (Domain == ComputeDomain::Fixed) {
            ZHLN::Assert(TemplatedDetail::HasPositiveExtent(fixedDispatchSize));
        }
        return true;
    }

    /// VK_EXT_descriptor_heap: null pipeline layout (spec-required) +
    /// set/binding -> heap mapping.
    [[nodiscard]] std::expected<void, ZHLN::Error> BuildHeap(
        VkDevice                                             device,
        const ZHLN_ShaderDesc&                               shader,
        const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
        uint32_t                                             indexPushOffset = 0
    ) noexcept {
        // VK_EXT_descriptor_heap: heap pipelines require layout ==
        // VK_NULL_HANDLE (VUID-VkComputePipelineCreateInfo-flags-11311). Per-
        // dispatch data travels through vkCmdPushDataEXT.
        if (!ReflectDispatchLayout(shader)) {
            return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
        }
        pipelineLayout      = {};
        heapIndexPushOffset = indexPushOffset;

        auto p_res = ComputePipelineBuilder().Shader(shader).Layout(VK_NULL_HANDLE).HeapMappings(mapping).Build(device);
        if (!p_res) {
            return std::unexpected(p_res.error());
        }
        pipeline = std::move(*p_res);
        return {};
    }

    /// Heap-mode specialized variants (same mapping covers every variant).
    [[nodiscard]] std::expected<void, ZHLN::Error> BuildHeapVariants(
        VkDevice                                             device,
        const ZHLN_ShaderDesc&                               shader,
        std::span<const VkSpecializationInfo>                specInfos,
        const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
        uint32_t                                             indexPushOffset = 0
    ) noexcept {
        if (!ReflectDispatchLayout(shader)) {
            return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
        }
        pipelineLayout      = {};
        heapIndexPushOffset = indexPushOffset;
        pipelines.clear();
        pipelines.reserve(specInfos.size());

        for (const auto& spec: specInfos) {
            auto p_res = ComputePipelineBuilder().Shader(shader).Layout(VK_NULL_HANDLE).HeapMappings(mapping).Specialization(&spec).Build(device);
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

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return pipeline.Valid() || !pipelines.empty();
    }

    [[nodiscard]] auto HasFixedDispatchDomain() const noexcept -> bool {
        return TemplatedDetail::HasPositiveExtent(fixedDispatchSize);
    }

    [[nodiscard]] auto HasHeapIndexPushOffset() const noexcept -> bool {
        return heapIndexPushOffset > 0;
    }

    void Bind(VkCommandBuffer cmd) const noexcept {
        ZHLN::Assert(cmd != VK_NULL_HANDLE);
        ZHLN::Assert(Valid());
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
    }

    void BindVariant(VkCommandBuffer cmd, uint32_t variantIdx) const noexcept {
        ZHLN::Assert(cmd != VK_NULL_HANDLE);
        ZHLN::Assert(variantIdx < pipelines.size());
        ZHLN::Assert(pipelines[variantIdx].Valid());
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[variantIdx].Get());
    }

    // Skinning only: legacy push constants (no descriptors involved).
    template <GpuTriviallyCopyable T>
    void PushConstants(VkCommandBuffer cmd, const T& pushData) const noexcept {
        ZHLN::Assert(cmd != VK_NULL_HANDLE);
        ZHLN::Assert(pipelineLayout.Valid());
        Push(cmd, pipelineLayout.Get(), VK_SHADER_STAGE_COMPUTE_BIT, pushData);
    }

    [[nodiscard]] auto MakeDispatchDesc(VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ) const noexcept
        -> TemplatedDetail::ComputeDispatchDesc {
        return {
            .cmd             = cmd,
            .threadGroupSize = threadGroupSize,
            .threadCountX    = threadCountX,
            .threadCountY    = threadCountY,
            .threadCountZ    = threadCountZ,
            .pipeline        = pipeline.Get(),
        };
    }

    [[nodiscard]] auto MakeFixedDispatchDesc(VkCommandBuffer cmd) const noexcept -> TemplatedDetail::ComputeDispatchDesc
        requires(Domain == ComputeDomain::Fixed)
    {
        ZHLN::Assert(TemplatedDetail::HasPositiveExtent(fixedDispatchSize));
        return MakeDispatchDesc(cmd, fixedDispatchSize[0], fixedDispatchSize[1], fixedDispatchSize[2]);
    }

    /// Dispatches a logical thread domain. Workgroup counts are derived from
    /// the reflected Slang `[numthreads]`; callers never repeat local sizes.
    /// Does not bind: the caller already bound a pipeline or variant.
    void DispatchThreads(VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        TemplatedDetail::RecordComputeDispatch(MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ));
    }

    /// Escape hatch for algorithms that intentionally specify raw workgroup
    /// counts. Prefer typed logical-domain dispatch for ordinary compute.
    static void DispatchGroups(VkCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept {
        ZHLN::Assert(cmd != VK_NULL_HANDLE);
        ZHLN::Assert(groupCountX > 0 && groupCountY > 0 && groupCountZ > 0);
        ZHLN::Vk::DispatchGroups(cmd, groupCountX, groupCountY, groupCountZ);
    }

    // Dispatch with push data only (BDA/skinning-style compute).
    template <GpuTriviallyCopyable T>
    void DispatchThreads(VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ, const T& pushData) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        ZHLN::Assert(Valid());
        ZHLN::Assert(pipelineLayout.Valid());
        auto desc          = MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ);
        desc.bind          = true;
        desc.legacyLayout  = pipelineLayout.Get();
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }

    // VK_EXT_descriptor_heap dispatch: heaps are bound on the command buffer,
    // per-dispatch data via vkCmdPushDataEXT at offset 0.
    template <HeapPassPushPayload T>
    void DispatchHeapThreads(
        const Context&  ctx,
        VkCommandBuffer cmd,
        uint32_t        threadCountX,
        uint32_t        threadCountY,
        uint32_t        threadCountZ,
        const T&        pushData
    ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        ZHLN::Assert(Valid());
        auto desc = MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ);
        desc.bind = true;
        desc.ctx  = &ctx;
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }

    void DispatchHeapThreads(const Context& ctx, VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        ZHLN::Assert(Valid());
        auto desc = MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ);
        desc.bind = true;
        desc.ctx  = &ctx;
        TemplatedDetail::RecordComputeDispatch(desc);
    }

    // Like DispatchHeapThreads, but also pushes the descriptor-index word
    // consumed by HEAP_WITH_PUSH_INDEX mappings (frame parity / mip level /
    // pass id).
    template <HeapPassPushPayload T>
    void DispatchHeapIndexedThreads(
        const Context&  ctx,
        VkCommandBuffer cmd,
        uint32_t        heapIndex,
        uint32_t        threadCountX,
        uint32_t        threadCountY,
        uint32_t        threadCountZ,
        const T&        pushData
    ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        ZHLN::Assert(Valid());
        ZHLN::Assert(heapIndexPushOffset > 0);
        auto desc            = MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ);
        desc.bind            = true;
        desc.ctx             = &ctx;
        desc.heapIndexOffset = heapIndexPushOffset;
        desc.heapIndex       = heapIndex;
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }

    void DispatchHeapIndexedThreads(
        const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ
    ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        ZHLN::Assert(Valid());
        ZHLN::Assert(heapIndexPushOffset > 0);
        auto desc            = MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ);
        desc.bind            = true;
        desc.ctx             = &ctx;
        desc.heapIndexOffset = heapIndexPushOffset;
        desc.heapIndex       = heapIndex;
        TemplatedDetail::RecordComputeDispatch(desc);
    }

    /// Dispatches the fixed logical domain declared by the Slang shader.
    /// Does not bind: the caller already bound a pipeline.
    void Dispatch(VkCommandBuffer cmd) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        TemplatedDetail::RecordComputeDispatch(MakeFixedDispatchDesc(cmd));
    }

    template <GpuTriviallyCopyable T>
    void Dispatch(VkCommandBuffer cmd, const T& pushData) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        ZHLN::Assert(Valid());
        ZHLN::Assert(pipelineLayout.Valid());
        auto desc         = MakeFixedDispatchDesc(cmd);
        desc.bind         = true;
        desc.legacyLayout = pipelineLayout.Get();
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }

    void DispatchHeap([[maybe_unused]] const Context& ctx, VkCommandBuffer cmd) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        ZHLN::Assert(Valid());
        auto desc = MakeFixedDispatchDesc(cmd);
        desc.bind = true;
        desc.ctx  = &ctx;
        TemplatedDetail::RecordComputeDispatch(desc);
    }

    template <HeapPassPushPayload T>
    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, const T& pushData) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        ZHLN::Assert(Valid());
        auto desc = MakeFixedDispatchDesc(cmd);
        desc.bind = true;
        desc.ctx  = &ctx;
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }

    void DispatchHeapIndexed(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        ZHLN::Assert(Valid());
        ZHLN::Assert(heapIndexPushOffset > 0);
        auto desc            = MakeFixedDispatchDesc(cmd);
        desc.bind            = true;
        desc.ctx             = &ctx;
        desc.heapIndexOffset = heapIndexPushOffset;
        desc.heapIndex       = heapIndex;
        TemplatedDetail::RecordComputeDispatch(desc);
    }

    template <HeapPassPushPayload T>
    void DispatchHeapIndexed(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, const T& pushData) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        ZHLN::Assert(Valid());
        ZHLN::Assert(heapIndexPushOffset > 0);
        auto desc            = MakeFixedDispatchDesc(cmd);
        desc.bind            = true;
        desc.ctx             = &ctx;
        desc.heapIndexOffset = heapIndexPushOffset;
        desc.heapIndex       = heapIndex;
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }
};

using DynamicComputePass = ComputePass<ComputeDomain::Dynamic>;
using FixedComputePass   = ComputePass<ComputeDomain::Fixed>;

/// Heap-mode compute pass with a reflected set layout (LayoutT) driving its
/// binding table; frame-parity slot spans via the pushed index word.
template <typename LayoutT, ComputeDomain Domain = ComputeDomain::Fixed>
struct DoubleBufferedComputePass {
    [[no_unique_address]] LayoutT layoutInstance {};
    Pipeline                      pipeline;
    HeapPassBindings              heapBindings;
    std::array<uint32_t, 3>       threadGroupSize {};
    std::array<uint32_t, 3>       fixedDispatchSize {};

    [[nodiscard]] bool BuildHeap(VkDevice device, HeapManager& heap, const ZHLN_ShaderDesc& shader, uint32_t indexPushOffset) noexcept {
        // Reflect the binding structure, [numthreads], and optional fixed
        // logical domain, then build a heap pipeline with a null layout + push
        // data.
        auto reflectedGroupSize = ReflectComputeThreadGroupSize(shader);
        if (!layoutInstance.Build(device, shader, VK_SHADER_STAGE_COMPUTE_BIT) || !reflectedGroupSize) {
            return false;
        }
        threadGroupSize = *reflectedGroupSize;

        auto reflectedFixed = ReflectComputeDispatchSize(shader);
        if constexpr (Domain == ComputeDomain::Fixed) {
            if (!reflectedFixed) {
                fixedDispatchSize = {};
                return false;
            }
            fixedDispatchSize = *reflectedFixed;
        } else {
            fixedDispatchSize = reflectedFixed.value_or(std::array<uint32_t, 3> {});
        }

        BuildHeapPassBindings(heap, layoutInstance.reflectedSets[0], 0, indexPushOffset, 2, heapBindings);

        auto p_res = ComputePipelineBuilder().Shader(shader).Layout(VK_NULL_HANDLE).HeapMappings(heapBindings.GetInfo()).Build(device);
        if (!p_res) {
            return false;
        }
        pipeline = std::move(*p_res);
        ZHLN::Assert(pipeline.Valid());
        ZHLN::Assert(TemplatedDetail::HasPositiveExtent(threadGroupSize));
        if constexpr (Domain == ComputeDomain::Fixed) {
            ZHLN::Assert(TemplatedDetail::HasPositiveExtent(fixedDispatchSize));
        }
        return true;
    }

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return pipeline.Valid();
    }

    [[nodiscard]] auto HasFixedDispatchDomain() const noexcept -> bool {
        return TemplatedDetail::HasPositiveExtent(fixedDispatchSize);
    }

    template <typename... Args>
    void WriteHeap(const Context& ctx, HeapManager& heap, uint32_t heapIndex, Args&&... args) const noexcept {
        heap.WriteBindings(ctx, heapBindings, heapIndex, std::forward<Args>(args)...);
    }

    [[nodiscard]] auto MakeDispatchDesc(VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ, const Context& ctx, uint32_t heapIndex)
        const noexcept -> TemplatedDetail::ComputeDispatchDesc {
        ZHLN::Assert(Valid());
        ZHLN::Assert(heapBindings.indexPushOffset > 0);
        return {
            .cmd             = cmd,
            .threadGroupSize = threadGroupSize,
            .threadCountX    = threadCountX,
            .threadCountY    = threadCountY,
            .threadCountZ    = threadCountZ,
            .pipeline        = pipeline.Get(),
            .bind            = true,
            .ctx             = &ctx,
            .heapIndexOffset = heapBindings.indexPushOffset,
            .heapIndex       = heapIndex,
        };
    }

    void DispatchHeapThreads(
        const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ
    ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        TemplatedDetail::RecordComputeDispatch(MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ, ctx, heapIndex));
    }

    template <HeapPassPushPayload T>
    void DispatchHeapThreads(
        const Context&  ctx,
        VkCommandBuffer cmd,
        uint32_t        heapIndex,
        uint32_t        threadCountX,
        uint32_t        threadCountY,
        uint32_t        threadCountZ,
        const T&        pushData
    ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        TemplatedDetail::RecordComputeDispatch(MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ, ctx, heapIndex), &pushData);
    }

    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        ZHLN::Assert(TemplatedDetail::HasPositiveExtent(fixedDispatchSize));
        TemplatedDetail::RecordComputeDispatch(MakeDispatchDesc(cmd, fixedDispatchSize[0], fixedDispatchSize[1], fixedDispatchSize[2], ctx, heapIndex));
    }

    template <HeapPassPushPayload T>
    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, uint32_t heapIndex, const T& pushData) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        ZHLN::Assert(TemplatedDetail::HasPositiveExtent(fixedDispatchSize));
        TemplatedDetail::RecordComputeDispatch(
            MakeDispatchDesc(cmd, fixedDispatchSize[0], fixedDispatchSize[1], fixedDispatchSize[2], ctx, heapIndex), &pushData
        );
    }
};

template <typename LayoutT>
using DynamicDoubleBufferedComputePass = DoubleBufferedComputePass<LayoutT, ComputeDomain::Dynamic>;

template <typename LayoutT>
using FixedDoubleBufferedComputePass = DoubleBufferedComputePass<LayoutT, ComputeDomain::Fixed>;

/// Builds a standalone descriptor-heap compute pass from compiled SPIR-V.
/// Dynamic-domain passes require `[numthreads]`. Fixed-domain passes also
/// require reflected `Dispatch.SizeX/Y/Z` metadata.
template <ComputeDomain Domain = ComputeDomain::Dynamic>
[[nodiscard]] inline auto CreateHeapComputePass(VkDevice device, const ZHLN_ShaderDesc& shader) noexcept -> std::expected<ComputePass<Domain>, Error> {
    if (shader.code == nullptr || shader.size == 0) {
        return std::unexpected(ShaderStageCreationError::ShaderLoadingFailed);
    }

    ComputePass<Domain> pass;
    if (!pass.ReflectDispatchLayout(shader)) {
        return std::unexpected(SpirvLayoutError::ModuleParseFailed);
    }

    return ComputePipelineBuilder().Shader(shader).Layout(VK_NULL_HANDLE).HeapPipeline().Build(device).transform([&](Pipeline&& pipeline) {
        pass.pipeline = std::move(pipeline);
        return std::move(pass);
    });
}

/// Same as above, with a PUSH_INDEX mapping table (bake / pass slot spans).
template <ComputeDomain Domain = ComputeDomain::Dynamic>
[[nodiscard]] inline auto CreateHeapComputePass(
    VkDevice                                             device,
    const ZHLN_ShaderDesc&                               shader,
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
    uint32_t                                             indexPushOffset
) noexcept -> std::expected<ComputePass<Domain>, Error> {
    if (shader.code == nullptr || shader.size == 0) {
        return std::unexpected(ShaderStageCreationError::ShaderLoadingFailed);
    }

    ComputePass<Domain> pass;
    if (!pass.ReflectDispatchLayout(shader)) {
        return std::unexpected(SpirvLayoutError::ModuleParseFailed);
    }
    return pass.BuildHeap(device, shader, mapping, indexPushOffset).transform([&] { return std::move(pass); });
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
 * Barriers are prepended (`_step > 0`), never appended: the first dispatch has
 * nothing to wait on, and the last dispatch cannot leave a trailing barrier
 * for the next pass. Cross-chain / cross-pass hazards use `MemoryBarrier` with
 * explicit access flags.
 *
 * `Step` takes the WriteBindings argument tail verbatim, because that order is
 * the shader's reflected binding order (see BuildHeapPassBindings), not anything
 * derivable from the pass's compile-time Usages list.
 */
class ComputeChain {
  public:
    constexpr ComputeChain(const Context& ctx, HeapManager& heap, VkCommandBuffer cmd, uint32_t frameIndex, uint32_t slotSpan) noexcept:
        _ctx(ctx), _heap(heap), _cmd(cmd), _frameIndex(frameIndex), _slotSpan(slotSpan) {
    }

    /// Bind and dispatch (sized from `extent`) one step of the chain. A
    /// compute-write -> compute-read barrier is recorded *before* every step
    /// after the first.
    template <typename PushT, typename... Args>
    [[gnu::always_inline]] void
        Step(DynamicComputePass& pass, const HeapPassBindings& bindings, VkExtent3D extent, const PushT& push, Args&&... args) noexcept {
        if (_step > 0) {
            MemoryBarrier(_cmd, BarrierStage::Compute, BarrierAccess::ShaderWrite, BarrierStage::Compute, BarrierAccess::ShaderRead);
        }
        const uint32_t slot = _frameIndex * _slotSpan + _step++;
        _heap.WriteBindings(_ctx, bindings, slot, std::forward<Args>(args)...);
        pass.DispatchHeapIndexedThreads(_ctx, _cmd, slot, extent.width, extent.height, 1, push);
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
