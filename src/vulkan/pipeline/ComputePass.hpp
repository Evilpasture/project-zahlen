// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
#include <Zahlen/Log.hpp>
#include "HeapBindings.hpp"

namespace ZHLN::Vk {

enum class ComputeDomain : uint8_t { Dynamic, Fixed };

template <typename T>
concept HeapPassPushPayload = GpuTriviallyCopyable<T>;

namespace TemplatedDetail {

[[nodiscard]] inline constexpr auto HasPositiveExtent(const std::array<uint32_t, 3>& extent) noexcept -> bool {
    return extent[0] > 0 && extent[1] > 0 && extent[2] > 0;
}

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
            PushData(desc.cmd, 0, *pushData);
        }
    }
    if (desc.heapIndexOffset > 0) {
        ZHLN::Assert(desc.ctx != nullptr);
        PushHeapIndex(desc.cmd, desc.heapIndexOffset, desc.heapIndex);
    }
    const auto workgroupCount = [](const uint32_t extent, const uint32_t localSize) noexcept {
        return extent / localSize + static_cast<uint32_t>(extent % localSize != 0U);
    };
    vkCmdDispatch(
        desc.cmd,
        workgroupCount(desc.threadCountX, desc.threadGroupSize[0]),
        workgroupCount(desc.threadCountY, desc.threadGroupSize[1]),
        workgroupCount(desc.threadCountZ, desc.threadGroupSize[2])
    );
}

}

template <ComputeDomain Domain = ComputeDomain::Dynamic>
struct ComputePass {
    PipelineLayout          pipelineLayout;
    Pipeline                pipeline;
    std::vector<Pipeline>   pipelines;
    std::array<uint32_t, 3> threadGroupSize {};
    std::array<uint32_t, 3> fixedDispatchSize {};

    uint32_t                heapIndexPushOffset = 0;

    [[nodiscard]] bool ReflectDispatchLayout(const ShaderDesc& shader) noexcept {
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

    [[nodiscard]] std::expected<void, Vk::Error> BuildHeap(
        VkDevice                                             device,
        const ShaderDesc&                               shader,
        const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
        uint32_t                                             indexPushOffset = 0,
        VkPipelineCache                                      cache           = VK_NULL_HANDLE
    ) noexcept {
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

    [[nodiscard]] std::expected<void, Vk::Error> BuildHeapVariants(
        VkDevice                                             device,
        const ShaderDesc&                               shader,
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

    void DispatchThreads(VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        TemplatedDetail::RecordComputeDispatch(MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ));
    }

    static void DispatchGroups(VkCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept {
        ZHLN::Assert(cmd != VK_NULL_HANDLE);
        ZHLN::Assert(groupCountX > 0 && groupCountY > 0 && groupCountZ > 0);
        vkCmdDispatch(cmd, groupCountX, groupCountY, groupCountZ);
    }

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

template <typename LayoutT, ComputeDomain Domain = ComputeDomain::Fixed>
struct DoubleBufferedComputePass {
    [[no_unique_address]] LayoutT layoutInstance {};
    Pipeline                      pipeline;
    HeapPassBindings              heapBindings;
    std::array<uint32_t, 3>       threadGroupSize {};
    std::array<uint32_t, 3>       fixedDispatchSize {};

    [[nodiscard]] std::expected<void, Vk::Error> BuildHeap(
        VkDevice               device,
        HeapManager&           heap,
        const ShaderDesc& shader,
        uint32_t               indexPushOffset,
        HeapLifecycle          lifecycle,
        VkPipelineCache        cache = VK_NULL_HANDLE
    ) noexcept {
        auto reflectedGroupSize = ReflectComputeThreadGroupSize(shader);
        if (!layoutInstance.Build(device, shader, VK_SHADER_STAGE_COMPUTE_BIT) || !reflectedGroupSize) {
            return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
        }
        threadGroupSize = *reflectedGroupSize;

        auto reflectedFixed = ReflectComputeDispatchSize(shader);
        if constexpr (Domain == ComputeDomain::Fixed) {
            if (!reflectedFixed) {
                fixedDispatchSize = {};
                return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
            }
            fixedDispatchSize = *reflectedFixed;
        } else {
            fixedDispatchSize = reflectedFixed.value_or(std::array<uint32_t, 3> {});
        }

        if (auto built = BuildHeapPassBindings(heap, layoutInstance.sets[0], 0, indexPushOffset, lifecycle, heapBindings); !built) {
            return std::unexpected(built.error());
        }

        auto p_res = ComputePipelineBuilder().Shader(shader).Layout(VK_NULL_HANDLE).HeapMappings(heapBindings.GetInfo()).Cache(cache).Build(device);
        if (!p_res) {
            return std::unexpected(p_res.error());
        }
        pipeline = std::move(*p_res);
        ZHLN::Assert(pipeline.Valid());
        ZHLN::Assert(TemplatedDetail::HasPositiveExtent(threadGroupSize));
        if constexpr (Domain == ComputeDomain::Fixed) {
            ZHLN::Assert(TemplatedDetail::HasPositiveExtent(fixedDispatchSize));
        }
        return {};
    }

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return pipeline.Valid();
    }

    [[nodiscard]] auto HasFixedDispatchDomain() const noexcept -> bool {
        return TemplatedDetail::HasPositiveExtent(fixedDispatchSize);
    }

    template <typename Declared, typename... Slots>
    [[nodiscard]] auto WriteHeapParameters(const Context& ctx, HeapManager& heap, const Slots&... slots) const noexcept -> HeapBlockBase {
        return heap.template WriteHeapParameters<Declared>(ctx, heapBindings, slots...);
    }

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

template <ComputeDomain Domain = ComputeDomain::Dynamic>
[[nodiscard]] inline auto CreateHeapComputePass(VkDevice device, const ShaderDesc& shader, VkPipelineCache cache = VK_NULL_HANDLE) noexcept
    -> std::expected<ComputePass<Domain>, Vk::Error> {
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

template <ComputeDomain Domain = ComputeDomain::Dynamic>
[[nodiscard]] inline auto CreateHeapComputePass(
    VkDevice                                             device,
    const ShaderDesc&                               shader,
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
    uint32_t                                             indexPushOffset,
    VkPipelineCache                                      cache = VK_NULL_HANDLE
) noexcept -> std::expected<ComputePass<Domain>, Vk::Error> {
    if (shader.code == nullptr || shader.size == 0) {
        return std::unexpected(ShaderStageCreationError::ShaderLoadingFailed);
    }

    ComputePass<Domain> pass;
    if (!pass.ReflectDispatchLayout(shader)) {
        return std::unexpected(SpirvLayoutError::ModuleParseFailed);
    }
    return pass.BuildHeap(device, shader, mapping, indexPushOffset, cache).transform([&] { return std::move(pass); });
}

class ComputeChain {
  public:
    constexpr ComputeChain(const Context& ctx, HeapManager& heap, VkCommandBuffer cmd) noexcept: _ctx(ctx), _heap(heap), _cmd(cmd) {
    }

    template <typename Declared, typename PushT, typename... Slots>
    [[gnu::always_inline]] void
        Step(DynamicComputePass& pass, const HeapPassBindings& bindings, VkExtent3D extent, const PushT& push, const Slots&... slots) noexcept {
        if (_step++ > 0) {
            MemoryBarrier(_cmd, BarrierStage::Compute, BarrierAccess::ShaderWrite, BarrierStage::Compute, BarrierAccess::ShaderRead);
        }
        const HeapBlockBase blockBase = _heap.template WriteHeapParameters<Declared>(_ctx, bindings, slots...);
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

}
