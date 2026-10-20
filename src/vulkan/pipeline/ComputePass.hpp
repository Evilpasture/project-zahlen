// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <array>
#include <span>
#include <Zahlen/Log.hpp>
#include "HeapBindings.hpp"
#include "HeapMappingBuilder.hpp"
#include "SpirvLayout.hpp"

namespace ZHLN::Vk {

enum class ComputeDomain : uint8_t { Dynamic, Fixed };

template <typename T>
concept HeapPassPushPayload = GpuTriviallyCopyable<T>;

namespace TemplatedDetail {

[[nodiscard]] inline constexpr auto HasPositiveExtent(std::span<const uint32_t, 3> extent) noexcept -> bool {
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
    uint32_t                heapIndexOffset = 0;
    uint32_t                heapIndex       = 0;
};

// Compile-time-workgroup dispatch: the threadgroup size is a non-type template
// parameter, so a zero dimension cannot compile and the group counts reduce to
// strength-reduced shifts. A dynamic zero extent (minimized window, empty
// particle buffer, 0-sized tile) is a valid recorded no-op -- no assertion, no
// [[assume(false)]], no division by zero (VUID-vkCmdDispatch-groupCountX-00386).
template <uint32_t LocalX, uint32_t LocalY, uint32_t LocalZ, typename PushT = std::monostate>
inline void RecordComputeDispatch(
    VkCommandBuffer cmd,
    VkPipeline      pipeline,
    uint32_t        threadsX,
    uint32_t        threadsY,
    uint32_t        threadsZ,
    uint32_t        heapIndexOffset = 0,
    uint32_t        heapIndex       = 0,
    const PushT*    pushData        = nullptr
) noexcept {
    static_assert(LocalX > 0 && LocalY > 0 && LocalZ > 0, "Threadgroup dimensions must be non-zero.");

    // 1. Guard against null command buffers or unbuilt pipelines
    if (cmd == VK_NULL_HANDLE || pipeline == VK_NULL_HANDLE) [[unlikely]] {
        return;
    }

    // 2. Spec compliance (VUID-vkCmdDispatch-groupCountX-00386): if any thread
    // count is 0, skip dispatch entirely.
    if (threadsX == 0 || threadsY == 0 || threadsZ == 0) [[unlikely]] {
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

    if constexpr (!std::is_same_v<PushT, std::monostate>) {
        if (pushData != nullptr) {
            PushData(cmd, 0, *pushData);
        }
    }

    if (heapIndexOffset > 0) {
        PushHeapIndex(cmd, heapIndexOffset, heapIndex);
    }

    // Compiler strength-reduces (x + 7) / 8 to ((x + 7) >> 3) at compile time.
    constexpr auto CalcGroups = [](uint32_t threads, uint32_t local) constexpr {
        return (threads + local - 1) / local;
    };

    vkCmdDispatch(cmd, CalcGroups(threadsX, LocalX), CalcGroups(threadsY, LocalY), CalcGroups(threadsZ, LocalZ));
}

// Legacy runtime-local-size path (fixed-domain and unmigrated passes): the
// same no-assert, no-UB treatment. ZHLN::Assert is [[assume(false)]] in ship
// builds, so a null handle or an empty extent is a recorded no-op instead.
template <typename PushT = std::monostate>
inline void RecordComputeDispatch(const ComputeDispatchDesc& desc, const PushT* pushData = nullptr) noexcept {
    if (desc.cmd == VK_NULL_HANDLE || desc.pipeline == VK_NULL_HANDLE) [[unlikely]] {
        return;
    }
    if (desc.threadCountX == 0 || desc.threadCountY == 0 || desc.threadCountZ == 0) [[unlikely]] {
        return;
    }
    if (desc.threadGroupSize[0] == 0 || desc.threadGroupSize[1] == 0 || desc.threadGroupSize[2] == 0) [[unlikely]] {
        return;
    }
    if (desc.bind) {
        vkCmdBindPipeline(desc.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, desc.pipeline);
    }
    if constexpr (!std::is_same_v<PushT, std::monostate>) {
        if (pushData != nullptr) {
            PushData(desc.cmd, 0, *pushData);
        }
    }
    if (desc.heapIndexOffset > 0) {
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
        if (!TemplatedDetail::HasPositiveExtent(threadGroupSize)) {
            threadGroupSize = {};
            fixedDispatchSize = {};
            return false;
        }
        if constexpr (Domain == ComputeDomain::Fixed) {
            if (!TemplatedDetail::HasPositiveExtent(fixedDispatchSize)) {
                threadGroupSize = {};
                fixedDispatchSize = {};
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::expected<void, ErrorCode> BuildHeap(
        VkDevice                                             device,
        const ShaderDesc&                               shader,
        const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
        uint32_t                                             indexPushOffset = 0,
        PipelineBinaryStore*                                   binaries        = nullptr
    ) noexcept {
        if (!ReflectDispatchLayout(shader)) {
            return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
        }
        heapIndexPushOffset = indexPushOffset;

        auto p_res = ComputePipelineBuilder().Shader(shader).HeapMappings(mapping).Binaries(binaries).Build(device);
        if (!p_res) {
            return std::unexpected(p_res.error());
        }
        pipeline = std::move(*p_res);
        return {};
    }

    [[nodiscard]] std::expected<void, ErrorCode> BuildHeapVariants(
        VkDevice                                             device,
        const ShaderDesc&                               shader,
        std::span<const VkSpecializationInfo>                specInfos,
        const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
        uint32_t                                             indexPushOffset = 0,
        PipelineBinaryStore*                                   binaries        = nullptr
    ) noexcept {
        if (!ReflectDispatchLayout(shader)) {
            return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
        }
        heapIndexPushOffset = indexPushOffset;
        pipelines.clear();
        pipelines.reserve(specInfos.size());

        for (const auto& spec: specInfos) {
            auto p_res = ComputePipelineBuilder().Shader(shader).HeapMappings(mapping).Specialization(&spec).Binaries(binaries).Build(device);
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
        if (cmd == VK_NULL_HANDLE || !Valid()) [[unlikely]] {
            return;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
    }

    void BindVariant(VkCommandBuffer cmd, uint32_t variantIdx) const noexcept {
        if (cmd == VK_NULL_HANDLE || variantIdx >= pipelines.size() || !pipelines[variantIdx].Valid()) [[unlikely]] {
            return;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[variantIdx].Get());
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
        return MakeDispatchDesc(cmd, fixedDispatchSize[0], fixedDispatchSize[1], fixedDispatchSize[2]);
    }

    void DispatchThreads(VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        TemplatedDetail::RecordComputeDispatch(MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ));
    }

    static void DispatchGroups(VkCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept {
        // A zero group count is a recorded no-op (VUID-vkCmdDispatch-groupCountX-00386).
        if (cmd == VK_NULL_HANDLE || groupCountX == 0 || groupCountY == 0 || groupCountZ == 0) [[unlikely]] {
            return;
        }
        vkCmdDispatch(cmd, groupCountX, groupCountY, groupCountZ);
    }

    template <ShaderProgram... Modules, HeapPassPushPayload T>
    void DispatchHeapThreads(
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
        if (!Valid() || heapIndexPushOffset != 0) [[unlikely]] {
            return;
        }
        auto desc = MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ);
        desc.bind = true;
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }

    void DispatchHeapThreads(VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        if (!Valid() || heapIndexPushOffset != 0) [[unlikely]] {
            return;
        }
        auto desc = MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ);
        desc.bind = true;
        TemplatedDetail::RecordComputeDispatch(desc);
    }

    template <ShaderProgram... Modules, HeapPassPushPayload T>
    void DispatchHeapIndexedThreads(
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
        if (!Valid() || heapIndexPushOffset == 0) [[unlikely]] {
            return;
        }
        auto desc            = MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ);
        desc.bind            = true;
        desc.heapIndexOffset = heapIndexPushOffset;
        desc.heapIndex       = blockBase.slot;
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }

    void DispatchHeapIndexedThreads(
        VkCommandBuffer cmd, HeapBlockBase blockBase, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ
    ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        if (!Valid() || heapIndexPushOffset == 0) [[unlikely]] {
            return;
        }
        auto desc            = MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ);
        desc.bind            = true;
        desc.heapIndexOffset = heapIndexPushOffset;
        desc.heapIndex       = blockBase.slot;
        TemplatedDetail::RecordComputeDispatch(desc);
    }

    void Dispatch(VkCommandBuffer cmd) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        TemplatedDetail::RecordComputeDispatch(MakeFixedDispatchDesc(cmd));
    }

    void DispatchHeap(VkCommandBuffer cmd) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        if (!Valid() || heapIndexPushOffset != 0) [[unlikely]] {
            return;
        }
        auto desc = MakeFixedDispatchDesc(cmd);
        desc.bind = true;
        TemplatedDetail::RecordComputeDispatch(desc);
    }

    template <ShaderProgram... Modules, HeapPassPushPayload T>
    void DispatchHeap(VkCommandBuffer cmd, const T& pushData) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        static_assert(sizeof...(Modules) > 0, "name the shader module(s) this dispatch is recorded for: DispatchHeap*<Shaders::Modules::X>(...)");
        static_assert(
            Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
            "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
        );
        if (!Valid() || heapIndexPushOffset != 0) [[unlikely]] {
            return;
        }
        auto desc = MakeFixedDispatchDesc(cmd);
        desc.bind = true;
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }

    void DispatchHeapIndexed(VkCommandBuffer cmd, HeapBlockBase blockBase) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        if (!Valid() || heapIndexPushOffset == 0) [[unlikely]] {
            return;
        }
        auto desc            = MakeFixedDispatchDesc(cmd);
        desc.bind            = true;
        desc.heapIndexOffset = heapIndexPushOffset;
        desc.heapIndex       = blockBase.slot;
        TemplatedDetail::RecordComputeDispatch(desc);
    }

    template <ShaderProgram... Modules, HeapPassPushPayload T>
    void DispatchHeapIndexed(VkCommandBuffer cmd, HeapBlockBase blockBase, const T& pushData) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        static_assert(sizeof...(Modules) > 0, "name the shader module(s) this dispatch is recorded for: DispatchHeap*<Shaders::Modules::X>(...)");
        static_assert(
            Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
            "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
        );
        if (!Valid() || heapIndexPushOffset == 0) [[unlikely]] {
            return;
        }
        auto desc            = MakeFixedDispatchDesc(cmd);
        desc.bind            = true;
        desc.heapIndexOffset = heapIndexPushOffset;
        desc.heapIndex       = blockBase.slot;
        TemplatedDetail::RecordComputeDispatch(desc, &pushData);
    }
};

using DynamicComputePass = ComputePass<ComputeDomain::Dynamic>;
using FixedComputePass   = ComputePass<ComputeDomain::Fixed>;

// A compute pipeline whose threadgroup size is a compile-time constant: the
// shader module is the template parameter, `kLocalSize` is what the module's
// own bytes declare (Vk::LocalSizeOf), and the dispatch math cannot divide by
// zero or assert on an empty extent -- both are impossible by construction.
// Mirrors the compile-time guarantees of GraphicsPipeline for compute.
template <ShaderProgram Module, typename PushConstants = void>
class TypedComputePass {
  public:
    static constexpr auto kLocalSize = LocalSizeOf<Module>;

    [[nodiscard]] auto Init(
        VkDevice                                             device,
        const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
        uint32_t                                             indexPushOffset = 0,
        PipelineBinaryStore*                                   binaries        = nullptr
    ) noexcept -> std::expected<void, ErrorCode> {
        _heapIndexPushOffset = indexPushOffset;

        const auto shader  = CreateShaderDesc<Module>();
        auto       p_res   = ComputePipelineBuilder().Shader(shader).HeapMappings(mapping).Binaries(binaries).Build(device);
        if (!p_res) {
            return std::unexpected(p_res.error());
        }
        _pipeline = std::move(*p_res);
        return {};
    }

    // Dispatch without push constants
    void DispatchThreads(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z, uint32_t heapIndex = 0) const noexcept
        requires std::is_void_v<PushConstants>
    {
        TemplatedDetail::RecordComputeDispatch<kLocalSize.x, kLocalSize.y, kLocalSize.z>(
            cmd, _pipeline.Get(), x, y, z, _heapIndexPushOffset, heapIndex
        );
    }

    // Dispatch with statically verified push constants
    void DispatchThreads(
        VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z, const PushConstants& push, uint32_t heapIndex = 0
    ) const noexcept
        requires(!std::is_void_v<PushConstants>)
    {
        static_assert(
            PushConstantLayoutMatchesAll<PushConstants, Module>(),
            "Push constant struct does not match the compute shader's push block reflection!"
        );

        TemplatedDetail::RecordComputeDispatch<kLocalSize.x, kLocalSize.y, kLocalSize.z>(
            cmd, _pipeline.Get(), x, y, z, _heapIndexPushOffset, heapIndex, &push
        );
    }

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _pipeline.Valid();
    }
    [[nodiscard]] auto Get() const noexcept -> VkPipeline {
        return _pipeline.Get();
    }

  private:
    Pipeline _pipeline;
    uint32_t _heapIndexPushOffset = 0;
};

template <typename LayoutT, ComputeDomain Domain = ComputeDomain::Fixed>
struct DoubleBufferedComputePass {
    [[no_unique_address]] LayoutT layoutInstance {};
    Pipeline                      pipeline;
    HeapPassBindings              heapBindings;
    std::array<uint32_t, 3>       threadGroupSize {};
    std::array<uint32_t, 3>       fixedDispatchSize {};

    [[nodiscard]] std::expected<void, ErrorCode> BuildHeap(
        VkDevice               device,
        HeapManager&           heap,
        const ShaderDesc& shader,
        uint32_t               indexPushOffset,
        HeapLifecycle          lifecycle,
        PipelineBinaryStore* binaries = nullptr
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

        HeapMappingArena mappingArena {heap};
        auto p_res = ComputePipelineBuilder().Shader(shader).HeapMappings(mappingArena.Map(heapBindings.mappings)).Binaries(binaries).Build(device);
        if (!p_res) {
            return std::unexpected(p_res.error());
        }
        pipeline = std::move(*p_res);
        if (!pipeline.Valid() || !TemplatedDetail::HasPositiveExtent(threadGroupSize)) [[unlikely]] {
            return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
        }
        if constexpr (Domain == ComputeDomain::Fixed) {
            if (!TemplatedDetail::HasPositiveExtent(fixedDispatchSize)) [[unlikely]] {
                return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
            }
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
    [[nodiscard]] auto WriteHeapParameters(HeapManager& heap, const Slots&... slots) const noexcept -> HeapBlockBase {
        return heap.template WriteHeapParameters<Declared>(heapBindings, slots...);
    }

    [[nodiscard]] auto MakeDispatchDesc(VkCommandBuffer cmd, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ, HeapBlockBase blockBase)
        const noexcept -> TemplatedDetail::ComputeDispatchDesc {
        if (!Valid() || heapBindings.indexPushOffset == 0) [[unlikely]] {
            return {};
        }
        return {
            .cmd             = cmd,
            .threadGroupSize = threadGroupSize,
            .threadCountX    = threadCountX,
            .threadCountY    = threadCountY,
            .threadCountZ    = threadCountZ,
            .pipeline        = pipeline.Get(),
            .bind            = true,
            .heapIndexOffset = heapBindings.indexPushOffset,
            .heapIndex       = blockBase.slot,
        };
    }

    void DispatchHeapThreads(
        VkCommandBuffer cmd, HeapBlockBase blockBase, uint32_t threadCountX, uint32_t threadCountY, uint32_t threadCountZ
    ) const noexcept
        requires(Domain == ComputeDomain::Dynamic)
    {
        TemplatedDetail::RecordComputeDispatch(MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ, blockBase));
    }

    template <ShaderProgram... Modules, HeapPassPushPayload T>
    void DispatchHeapThreads(
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
        TemplatedDetail::RecordComputeDispatch(MakeDispatchDesc(cmd, threadCountX, threadCountY, threadCountZ, blockBase), &pushData);
    }

    void DispatchHeap(VkCommandBuffer cmd, HeapBlockBase blockBase) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        TemplatedDetail::RecordComputeDispatch(
            MakeDispatchDesc(cmd, fixedDispatchSize[0], fixedDispatchSize[1], fixedDispatchSize[2], blockBase)
        );
    }

    template <ShaderProgram... Modules, HeapPassPushPayload T>
    void DispatchHeap(VkCommandBuffer cmd, HeapBlockBase blockBase, const T& pushData) const noexcept
        requires(Domain == ComputeDomain::Fixed)
    {
        static_assert(
            Vk::PushConstantLayoutMatchesAll<T, Modules...>(),
            "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
        );
        TemplatedDetail::RecordComputeDispatch(
            MakeDispatchDesc(cmd, fixedDispatchSize[0], fixedDispatchSize[1], fixedDispatchSize[2], blockBase), &pushData
        );
    }
};

template <typename LayoutT>
using DynamicDoubleBufferedComputePass = DoubleBufferedComputePass<LayoutT, ComputeDomain::Dynamic>;

template <typename LayoutT>
using FixedDoubleBufferedComputePass = DoubleBufferedComputePass<LayoutT, ComputeDomain::Fixed>;

template <ComputeDomain Domain = ComputeDomain::Dynamic>
[[nodiscard]] inline auto CreateHeapComputePass(VkDevice device, const ShaderDesc& shader, PipelineBinaryStore* binaries = nullptr) noexcept
    -> std::expected<ComputePass<Domain>, ErrorCode> {
    if (shader.code == nullptr || shader.size == 0) {
        return std::unexpected(ShaderStageCreationError::ShaderLoadingFailed);
    }

    ComputePass<Domain> pass;
    if (!pass.ReflectDispatchLayout(shader)) {
        return std::unexpected(SpirvLayoutError::ModuleParseFailed);
    }

    return ComputePipelineBuilder().Shader(shader).Binaries(binaries).Build(device).transform([&](Pipeline&& pipeline) {
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
    PipelineBinaryStore*                                   binaries = nullptr
) noexcept -> std::expected<ComputePass<Domain>, ErrorCode> {
    if (shader.code == nullptr || shader.size == 0) {
        return std::unexpected(ShaderStageCreationError::ShaderLoadingFailed);
    }

    ComputePass<Domain> pass;
    if (!pass.ReflectDispatchLayout(shader)) {
        return std::unexpected(SpirvLayoutError::ModuleParseFailed);
    }
    return pass.BuildHeap(device, shader, mapping, indexPushOffset, binaries).transform([&] { return std::move(pass); });
}

class ComputeChain {
  public:
    constexpr ComputeChain(HeapManager& heap, VkCommandBuffer cmd) noexcept: _heap(heap), _cmd(cmd) {
    }

    template <typename Declared, typename PushT, typename... Slots>
    [[gnu::always_inline]] void
        Step(DynamicComputePass& pass, const HeapPassBindings& bindings, VkExtent3D extent, const PushT& push, const Slots&... slots) noexcept {
        if (_step++ > 0) {
            MemoryBarrier(_cmd, BarrierStage::Compute, BarrierAccess::ShaderWrite, BarrierStage::Compute, BarrierAccess::ShaderRead);
        }
        const HeapBlockBase blockBase = _heap.template WriteHeapParameters<Declared>(bindings, slots...);
        Declared::DispatchHeapIndexed(pass, _cmd, blockBase, extent.width, extent.height, 1, push);
    }

    [[nodiscard]] constexpr auto StepCount() const noexcept -> uint32_t {
        return _step;
    }

  private:
    HeapManager&    _heap;
    VkCommandBuffer _cmd;
    uint32_t        _step = 0;
};

}
