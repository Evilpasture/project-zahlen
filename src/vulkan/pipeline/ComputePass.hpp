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
#include "HeapBindings.hpp" // PushHeapIndex and HeapPassBindings: the heap push data this pass writes

namespace ZHLN::Vk {

enum class ComputeDomain : uint8_t { Dynamic, Fixed };

// A push payload, to the mechanism: a blob that can be memcpy'd at an
// offset. How much of the blob the pass may occupy before it reaches the
// schema's frame addresses is the schema's question -- src/render pins its
// payload types against that bound (GpuAbi.hpp).
template <typename T>
concept HeapPassPushPayload = GpuTriviallyCopyable<T>;

namespace TemplatedDetail {

[[nodiscard]] inline constexpr auto HasPositiveExtent(const std::array<uint32_t, 3>& extent) noexcept -> bool {
    return extent[0] > 0 && extent[1] > 0 && extent[2] > 0;
}

// Bind (optional), push (optional), then one vkCmdDispatch via Vk::Dispatch.
// Heap index 0-offset means "do not push an index word".
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

    // The mapping's push-data offset when this pass's table is PUSH_INDEX, and
    // 0 when it is not (the scene registry's constant-offset / push-address
    // tables). The pushed word is what identifies the dispatch's block, so the
    // non-indexed dispatch paths assert this is 0: with a PUSH_INDEX table they
    // would resolve to whatever a previous dispatch left at that offset.
    uint32_t                heapIndexPushOffset = 0;

    // Reflects Slang's `[numthreads]` and optional fixed dispatch metadata
    // from the compiled compute entry point. Fixed-domain passes require the
    // shader to publish Dispatch.SizeX/Y/Z; dynamic passes only require
    // `[numthreads]`.
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

    // VK_EXT_descriptor_heap: null pipeline layout (spec-required) +
    // set/binding -> heap mapping.
    [[nodiscard]] std::expected<void, ZHLN::ErrorCode> BuildHeap(
        VkDevice                                             device,
        const ZHLN_ShaderDesc&                               shader,
        const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
        uint32_t                                             indexPushOffset = 0,
        VkPipelineCache                                      cache           = VK_NULL_HANDLE
    ) noexcept {
        // VK_EXT_descriptor_heap: heap pipelines require layout ==
        // VK_NULL_HANDLE (VUID-VkComputePipelineCreateInfo-flags-11311). Per-
        // dispatch data travels through vkCmdPushDataEXT.
        if (!ReflectDispatchLayout(shader)) {
            return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
        }
        pipelineLayout      = {};
        heapIndexPushOffset = indexPushOffset;

        auto p_res = ComputePipelineBuilder().Shader(shader).Layout(VK_NULL_HANDLE).HeapMappings(mapping).Cache(cache).Build(device);
        if (!p_res) {
            return std::unexpected(p_res.error());
        }
        pipeline = std::move(*p_res);
        return {};
    }

    // Heap-mode specialized variants (same mapping covers every variant).
    [[nodiscard]] std::expected<void, ZHLN::ErrorCode> BuildHeapVariants(
        VkDevice                                             device,
        const ZHLN_ShaderDesc&                               shader,
        std::span<const VkSpecializationInfo>                specInfos,
        const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
        uint32_t                                             indexPushOffset = 0,
        VkPipelineCache                                      cache           = VK_NULL_HANDLE
    ) noexcept {
        if (!ReflectDispatchLayout(shader)) {
            return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
        }
        pipelineLayout      = {};
        heapIndexPushOffset = indexPushOffset;
        pipelines.clear();
        pipelines.reserve(specInfos.size());

        for (const auto& spec: specInfos) {
            auto p_res = ComputePipelineBuilder().Shader(shader).Layout(VK_NULL_HANDLE).HeapMappings(mapping).Specialization(&spec).Cache(cache).Build(device);
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

    // Skinning only: legacy push constants (no descriptors involved). The
    // modules are the ones whose blocks the bytes are: this is the same
    // contract the heap dispatch entry points state, for the one path that
    // still writes a pipeline-layout push-constant range.
    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void PushConstants(VkCommandBuffer cmd, const T& pushData) const noexcept {
        static_assert(sizeof...(Modules) > 0, "name the shader module(s) this push struct is written for: PushConstants<Shaders::Modules::X>(...)");
        static_assert(
            Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
            "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
        );
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

    // Dispatches a logical thread domain. Workgroup counts are derived from
    // the reflected Slang `[numthreads]`; callers never repeat local sizes.
    // Does not bind: the caller already bound a pipeline or variant.
    void DispatchThreads(VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        TemplatedDetail::RecordComputeDispatch(MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ));
    }

    // Escape hatch for algorithms that intentionally specify raw workgroup
    // counts. Prefer typed logical-domain dispatch for ordinary compute.
    static void DispatchGroups(VkCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept {
        ZHLN::Assert(cmd != VK_NULL_HANDLE);
        ZHLN::Assert(groupCountX > 0 && groupCountY > 0 && groupCountZ > 0);
        ZHLN::Vk::DispatchGroups(cmd, groupCountX, groupCountY, groupCountZ);
    }

    // Dispatch with push data only (BDA/skinning-style compute). The modules
    // are the ones whose blocks these bytes are, named at the call site exactly
    // as the heap entry points require.
    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DispatchThreads(VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ, const T& pushData) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        static_assert(sizeof...(Modules) > 0, "name the shader module(s) this dispatch is recorded for: DispatchThreads<Shaders::Modules::X>(...)");
        static_assert(
            Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
            "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
        );
        ZHLN::Assert(Valid());
        ZHLN::Assert(pipelineLayout.Valid());
        auto desc          = MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ);
        desc.bind          = true;
        desc.legacyLayout  = pipelineLayout.Get();
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }

    // VK_EXT_descriptor_heap dispatch: heaps are bound on the command buffer,
    // per-dispatch data via vkCmdPushDataEXT at offset 0.
    template <ShaderProgram... Modules, HeapPassPushPayload T>
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
        static_assert(sizeof...(Modules) > 0, "name the shader module(s) this dispatch is recorded for: DispatchHeap*<Shaders::Modules::X>(...)");
        static_assert(
            Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
            "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
        );
        ZHLN::Assert(Valid());
        ZHLN::Assert(heapIndexPushOffset == 0);
        auto desc = MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ);
        desc.bind = true;
        desc.ctx  = &ctx;
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }

    void DispatchHeapThreads(const Context& ctx, VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        ZHLN::Assert(Valid());
        ZHLN::Assert(heapIndexPushOffset == 0);
        auto desc = MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ);
        desc.bind = true;
        desc.ctx  = &ctx;
        TemplatedDetail::RecordComputeDispatch(desc);
    }

    // Like DispatchHeapThreads, but also pushes the index word consumed by
    // HEAP_WITH_PUSH_INDEX mappings. `blockBase` is not an ordinal: it is the
    // base slot the block's WriteHeapParameters call returned, which the
    // slot-independent mapping adds the binding's ordinal to.
    template <ShaderProgram... Modules, HeapPassPushPayload T>
    void DispatchHeapIndexedThreads(
        const Context&  ctx,
        VkCommandBuffer cmd,
        HeapBlockBase   blockBase,
        uint32_t        threadCountX,
        uint32_t        threadCountY,
        uint32_t        threadCountZ,
        const T&        pushData
    ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        static_assert(sizeof...(Modules) > 0, "name the shader module(s) this dispatch is recorded for: DispatchHeap*<Shaders::Modules::X>(...)");
        static_assert(
            Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
            "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
        );
        ZHLN::Assert(Valid());
        ZHLN::Assert(heapIndexPushOffset > 0);
        auto desc            = MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ);
        desc.bind            = true;
        desc.ctx             = &ctx;
        desc.heapIndexOffset = heapIndexPushOffset;
        desc.heapIndex       = blockBase.slot;
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }

    void DispatchHeapIndexedThreads(
        const Context& ctx, VkCommandBuffer cmd, HeapBlockBase blockBase, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ
    ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        ZHLN::Assert(Valid());
        ZHLN::Assert(heapIndexPushOffset > 0);
        auto desc            = MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ);
        desc.bind            = true;
        desc.ctx             = &ctx;
        desc.heapIndexOffset = heapIndexPushOffset;
        desc.heapIndex       = blockBase.slot;
        TemplatedDetail::RecordComputeDispatch(desc);
    }

    // Dispatches the fixed logical domain declared by the Slang shader.
    // Does not bind: the caller already bound a pipeline.
    void Dispatch(VkCommandBuffer cmd) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        TemplatedDetail::RecordComputeDispatch(MakeFixedDispatchDesc(cmd));
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void Dispatch(VkCommandBuffer cmd, const T& pushData) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        static_assert(sizeof...(Modules) > 0, "name the shader module(s) this dispatch is recorded for: Dispatch<Shaders::Modules::X>(...)");
        static_assert(
            Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
            "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
        );
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
        ZHLN::Assert(heapIndexPushOffset == 0);
        auto desc = MakeFixedDispatchDesc(cmd);
        desc.bind = true;
        desc.ctx  = &ctx;
        TemplatedDetail::RecordComputeDispatch(desc);
    }

    template <ShaderProgram... Modules, HeapPassPushPayload T>
    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, const T& pushData) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        static_assert(sizeof...(Modules) > 0, "name the shader module(s) this dispatch is recorded for: DispatchHeap*<Shaders::Modules::X>(...)");
        static_assert(
            Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
            "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
        );
        ZHLN::Assert(Valid());
        ZHLN::Assert(heapIndexPushOffset == 0);
        auto desc = MakeFixedDispatchDesc(cmd);
        desc.bind = true;
        desc.ctx  = &ctx;
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }

    void DispatchHeapIndexed(const Context& ctx, VkCommandBuffer cmd, HeapBlockBase blockBase) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        ZHLN::Assert(Valid());
        ZHLN::Assert(heapIndexPushOffset > 0);
        auto desc            = MakeFixedDispatchDesc(cmd);
        desc.bind            = true;
        desc.ctx             = &ctx;
        desc.heapIndexOffset = heapIndexPushOffset;
        desc.heapIndex       = blockBase.slot;
        TemplatedDetail::RecordComputeDispatch(desc);
    }

    template <ShaderProgram... Modules, HeapPassPushPayload T>
    void DispatchHeapIndexed(const Context& ctx, VkCommandBuffer cmd, HeapBlockBase blockBase, const T& pushData) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        static_assert(sizeof...(Modules) > 0, "name the shader module(s) this dispatch is recorded for: DispatchHeap*<Shaders::Modules::X>(...)");
        static_assert(
            Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
            "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
        );
        ZHLN::Assert(Valid());
        ZHLN::Assert(heapIndexPushOffset > 0);
        auto desc            = MakeFixedDispatchDesc(cmd);
        desc.bind            = true;
        desc.ctx             = &ctx;
        desc.heapIndexOffset = heapIndexPushOffset;
        desc.heapIndex       = blockBase.slot;
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }
};

using DynamicComputePass = ComputePass<ComputeDomain::Dynamic>;
using FixedComputePass   = ComputePass<ComputeDomain::Fixed>;

// Heap-mode compute pass with a reflected set layout (LayoutT) driving its
// binding table; the pushed index word carries the base slot of the block the
// write just allocated.
template <typename LayoutT, ComputeDomain Domain = ComputeDomain::Fixed>
struct DoubleBufferedComputePass {
    [[no_unique_address]] LayoutT layoutInstance {};
    Pipeline                      pipeline;
    HeapPassBindings              heapBindings;
    std::array<uint32_t, 3>       threadGroupSize {};
    std::array<uint32_t, 3>       fixedDispatchSize {};

    // Reflects the binding structure and bakes the mapping table itself.
    // `lifecycle` decides which partition the pass's blocks are allocated from
    // (see HeapLifecycle): the frame's, unless the caller records the pass
    // outside the frame loop.
    [[nodiscard]] bool BuildHeap(
        VkDevice               device,
        HeapManager&           heap,
        const ZHLN_ShaderDesc& shader,
        uint32_t               indexPushOffset,
        HeapLifecycle          lifecycle,
        VkPipelineCache        cache = VK_NULL_HANDLE
    ) noexcept {
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

        if (auto built = BuildHeapPassBindings(heap, layoutInstance.sets[0], 0, indexPushOffset, lifecycle, heapBindings); !built) {
            return false;
        }

        auto p_res = ComputePipelineBuilder().Shader(shader).Layout(VK_NULL_HANDLE).HeapMappings(heapBindings.GetInfo()).Cache(cache).Build(device);
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

    // Writes the named descriptor values (Vk::Slot<"binding">(value)) into a
    // fresh transient block and returns its base, which the dispatch pushes.
    // Each name is matched against the shader's reflected binding names, so
    // argument order carries no meaning; `Declared` is the pass's descriptor
    // block (the generated <ShaderBindings.hpp>) and is what turns a misspelled name into a
    // compile error; see HeapManager::WriteHeapParameters.
    template <typename Declared, typename... Slots>
    [[nodiscard]] auto WriteHeapParameters(const Context& ctx, HeapManager& heap, const Slots&... slots) const noexcept -> HeapBlockBase {
        return heap.template WriteHeapParameters<Declared>(ctx, heapBindings, slots...);
    }

    // `blockBase` is the base slot WriteHeapParameters returned for this
    // dispatch, and reaching the shader as its base slot is the point: the
    // mapping adds only the binding's ordinal.
    [[nodiscard]] auto MakeDispatchDesc(VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ, const Context& ctx, HeapBlockBase blockBase)
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
            .heapIndex       = blockBase.slot,
        };
    }

    void DispatchHeapThreads(
        const Context& ctx, VkCommandBuffer cmd, HeapBlockBase blockBase, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ
    ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        TemplatedDetail::RecordComputeDispatch(MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ, ctx, blockBase));
    }

    template <ShaderProgram... Modules, HeapPassPushPayload T>
    void DispatchHeapThreads(
        const Context&  ctx,
        VkCommandBuffer cmd,
        HeapBlockBase   blockBase,
        uint32_t        threadCountX,
        uint32_t        threadCountY,
        uint32_t        threadCountZ,
        const T&        pushData
    ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        static_assert(
            Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
            "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
        );
        TemplatedDetail::RecordComputeDispatch(MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ, ctx, blockBase), &pushData);
    }

    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, HeapBlockBase blockBase) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        ZHLN::Assert(TemplatedDetail::HasPositiveExtent(fixedDispatchSize));
        TemplatedDetail::RecordComputeDispatch(
            MakeDispatchDesc(cmd, fixedDispatchSize[0], fixedDispatchSize[1], fixedDispatchSize[2], ctx, blockBase)
        );
    }

    template <ShaderProgram... Modules, HeapPassPushPayload T>
    void DispatchHeap(const Context& ctx, VkCommandBuffer cmd, HeapBlockBase blockBase, const T& pushData) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        static_assert(
            Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
            "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
        );
        ZHLN::Assert(TemplatedDetail::HasPositiveExtent(fixedDispatchSize));
        TemplatedDetail::RecordComputeDispatch(
            MakeDispatchDesc(cmd, fixedDispatchSize[0], fixedDispatchSize[1], fixedDispatchSize[2], ctx, blockBase), &pushData
        );
    }
};

template <typename LayoutT>
using DynamicDoubleBufferedComputePass = DoubleBufferedComputePass<LayoutT, ComputeDomain::Dynamic>;

template <typename LayoutT>
using FixedDoubleBufferedComputePass = DoubleBufferedComputePass<LayoutT, ComputeDomain::Fixed>;

// Builds a standalone descriptor-heap compute pass from compiled SPIR-V.
// Dynamic-domain passes require `[numthreads]`. Fixed-domain passes also
// require reflected `Dispatch.SizeX/Y/Z` metadata.
template <ComputeDomain Domain = ComputeDomain::Dynamic>
[[nodiscard]] inline auto CreateHeapComputePass(VkDevice device, const ZHLN_ShaderDesc& shader, VkPipelineCache cache = VK_NULL_HANDLE) noexcept
    -> std::expected<ComputePass<Domain>, ErrorCode> {
    if (shader.code == nullptr || shader.size == 0) {
        return std::unexpected(ShaderStageCreationError::ShaderLoadingFailed);
    }

    ComputePass<Domain> pass;
    if (!pass.ReflectDispatchLayout(shader)) {
        return std::unexpected(SpirvLayoutError::ModuleParseFailed);
    }

    return ComputePipelineBuilder().Shader(shader).Layout(VK_NULL_HANDLE).HeapPipeline().Cache(cache).Build(device).transform([&](Pipeline&& pipeline) {
        pass.pipeline = std::move(pipeline);
        return std::move(pass);
    });
}

// Same as above, with a PUSH_INDEX mapping table (bake / per-dispatch blocks).
template <ComputeDomain Domain = ComputeDomain::Dynamic>
[[nodiscard]] inline auto CreateHeapComputePass(
    VkDevice                                             device,
    const ZHLN_ShaderDesc&                               shader,
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
    uint32_t                                             indexPushOffset,
    VkPipelineCache                                      cache = VK_NULL_HANDLE
) noexcept -> std::expected<ComputePass<Domain>, ErrorCode> {
    if (shader.code == nullptr || shader.size == 0) {
        return std::unexpected(ShaderStageCreationError::ShaderLoadingFailed);
    }

    ComputePass<Domain> pass;
    if (!pass.ReflectDispatchLayout(shader)) {
        return std::unexpected(SpirvLayoutError::ModuleParseFailed);
    }
    return pass.BuildHeap(device, shader, mapping, indexPushOffset, cache).transform([&] { return std::move(pass); });
}

/**
 * @brief Records a chain of dependent compute dispatches through one heap table.
 *
 * Two things every multi-dispatch pass was hand-rolling:
 *
 *  - Block allocation. Heap descriptor writes are immediate host writes, so
 *    each in-frame step must dispatch through a block no later step will
 *    overwrite: Step allocates one from the frame's transient partition, which
 *    is what a step used to get handed as a reserved variant index.
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
 * `Step` takes the pass's named descriptor values (Vk::Slot<"binding">(value),
 * DescriptorWrites.hpp) verbatim: the names are the shader's own binding names,
 * which not even the pass's compile-time Usages list knows. `Declared` names the
 * block those values belong to (<ShaderBindings.hpp>), so a step cannot misspell a
 * binding any more quietly than a direct write can.
 */
class ComputeChain {
  public:
    constexpr ComputeChain(const Context& ctx, HeapManager& heap, VkCommandBuffer cmd) noexcept: _ctx(ctx), _heap(heap), _cmd(cmd) {
    }

    // Bind and dispatch (sized from `extent`) one step of the chain. A
    // compute-write -> compute-read barrier is recorded *before* every step
    // after the first.
    template <typename Declared, typename PushT, typename... Slots>
    [[gnu::always_inline]] void
        Step(DynamicComputePass& pass, const HeapPassBindings& bindings, VkExtent3D extent, const PushT& push, const Slots&... slots) noexcept {
        if (_step++ > 0) {
            MemoryBarrier(_cmd, BarrierStage::Compute, BarrierAccess::ShaderWrite, BarrierStage::Compute, BarrierAccess::ShaderRead);
        }
        // Each step gets its own block: the descriptors of the steps before it
        // are still in flight while this one is recorded.
        const HeapBlockBase blockBase = _heap.template WriteHeapParameters<Declared>(_ctx, bindings, slots...);
        // The step dispatches through the set, so the modules the declaration
        // names are the modules the entry point holds the payload against.
        Declared::DispatchHeapIndexed(pass, _ctx, _cmd, blockBase, extent.width, extent.height, 1, push);
    }

    [[nodiscard]] constexpr auto StepCount() const noexcept -> uint32_t {
        return _step;
    }

  private:
    const Context&  _ctx;
    HeapManager&    _heap;
    VkCommandBuffer _cmd;
    uint32_t        _step = 0;
};

} // namespace ZHLN::Vk
