// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "PassExecution.hpp"
#include <vkd3d/Barriers.hpp>
#include <vkd3d/VulkanConversions.hpp>
#include <vkd3d/graph/PassDecl.hpp>
#include <vkd3d/graph/StateTable.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <array>
#include <string_view>
#include <tuple>
#include <utility>

namespace ZHLN::Vk {

namespace TemplatedDetail {

template <typename List, typename T>
struct AppendUnique;
template <typename List1, typename List2>
struct MergeLists;
template <typename UsagesList>
struct ExtractResources;

template <typename Accumulated, typename... Lists>
struct MergeFold;

template <typename...>
inline constexpr bool DependentFalse = false;

}

template <typename... SubPasses>
struct ParallelPass {
    static constexpr auto name = VkD3D::ResourceName("ParallelPassGroup");
    using Usages = typename TemplatedDetail::MergeFold<VkD3D::TypeList<>, typename SubPasses::Usages...>::type;

    static constexpr bool   is_fork    = true;
    static constexpr size_t kBodyCount = sizeof...(SubPasses);

    std::tuple<SubPasses...> subPasses;

    constexpr explicit ParallelPass(SubPasses&&... passes) noexcept: subPasses(std::forward<SubPasses>(passes)...) {
    }

    [[nodiscard]] auto Bodies() const noexcept -> std::array<ForkCall, sizeof...(SubPasses)> {
        return std::apply(
            [](const SubPasses&... pass) -> std::array<ForkCall, sizeof...(SubPasses)> { return {ForkCall {pass}...}; }, subPasses
        );
    }
};

template <typename... SubPasses>
constexpr auto Fork(SubPasses&&... passes) {
    return ParallelPass<std::decay_t<SubPasses>...>(std::forward<SubPasses>(passes)...);
}

namespace TemplatedDetail {

template <typename InList, typename OutList, template <typename> class Predicate>
struct FilterImpl;

template <typename List, template <typename> class Predicate>
using Filter = typename FilterImpl<List, VkD3D::TypeList<>, Predicate>::type;

template <typename... Passes>
struct CollectAllResources;

template <typename Resources, typename Pass>
struct PassFootprint {
    static constexpr size_t kMaxResources = 64;

    template <typename Usages, size_t I>
    static constexpr auto Fold(std::array<uint64_t, 2> accumulated) noexcept {
        using Usage            = typename Usages::template type<I>;
        constexpr size_t index = VkD3D::TemplatedDetail::GetResourceIndex<Resources, typename Usage::Resource>();
        static_assert(index < kMaxResources, "The frame graph's hazard masks hold 64 resources; widen them before the graph reaches 65.");
        const uint64_t bit = uint64_t {1} << index;
        accumulated[VkD3D::Has(Usage::access, VkD3D::TemplatedDetail::WriteMask) ? 0 : 1] |= bit;
        return accumulated;
    }

    template <typename Usages, size_t... Is>
    static consteval auto Build(std::index_sequence<Is...>) noexcept {
        std::array<uint64_t, 2> accumulated {};
        ((accumulated = Fold<Usages, Is>(accumulated)), ...);
        return accumulated;
    }

    static constexpr std::array<uint64_t, 2> masks = Build<typename Pass::Usages>(std::make_index_sequence<Pass::Usages::size> {});
};

template <typename P>
struct IsForkablePass {
    using Usages      = typename P::Usages;
    using ColorWrites = Filter<Usages, VkD3D::TemplatedDetail::IsColorAttachment>;
    using DepthWrites = Filter<Usages, VkD3D::TemplatedDetail::IsDepthAttachment>;

    static constexpr bool is_graphics = (ColorWrites::size > 0) || (DepthWrites::size > 0);
    static constexpr bool value       = !is_graphics || std::is_invocable_v<P, VkCommandBuffer>;
};

template <typename... S>
struct IsForkablePass<ParallelPass<S...>>: std::false_type {};

template <size_t Capacity>
struct ForkPlan {
    std::array<size_t, Capacity> offsets {};
    std::array<size_t, Capacity> lengths {};
    size_t                       numRuns = 0;
};

template <typename Resources, typename... Passes>
consteval auto ComputeForkPlan() noexcept {
    constexpr size_t            N = sizeof...(Passes);
    ForkPlan<(N > 0 ? N : 1)> plan {};
    if constexpr (N > 0) {
        constexpr std::array<bool, N>                    forkable   = {IsForkablePass<Passes>::value...};
        constexpr std::array<std::array<uint64_t, 2>, N> footprints = {PassFootprint<Resources, Passes>::masks...};

        size_t i = 0;
        while (i < N) {
            size_t   len       = 1;
            uint64_t accWrites = footprints[i][0];
            uint64_t accReads  = footprints[i][1];
            if (forkable[i]) {
                while (i + len < N && forkable[i + len]) {
                    const uint64_t w = footprints[i + len][0];
                    const uint64_t r = footprints[i + len][1];
                    if (((accWrites & (w | r)) != 0) || ((accReads & w) != 0)) {
                        break;
                    }
                    accWrites |= w;
                    accReads |= r;
                    ++len;
                }
            }
            plan.offsets[plan.numRuns] = i;
            plan.lengths[plan.numRuns] = len;
            ++plan.numRuns;
            i += len;
        }
    }
    return plan;
}

template <size_t Offset, size_t Len, typename... Passes>
consteval auto ForkRunTypeHelper() {
    if constexpr (Len == 1) {
        return std::type_identity<Passes...[Offset]> {};
    } else {
        return []<size_t... Ks>(std::index_sequence<Ks...>) {
            return std::type_identity<ParallelPass<Passes...[Offset + Ks]...>> {};
        }(std::make_index_sequence<Len> {});
    }
}

template <typename Resources, typename... Passes>
consteval auto AutoForkTypeHelper() {
    if constexpr (sizeof...(Passes) == 0) {
        return std::type_identity<VkD3D::TypeList<>> {};
    } else {
        constexpr auto plan = ComputeForkPlan<Resources, Passes...>();
        return [&]<size_t... Rs>(std::index_sequence<Rs...>) {
            return std::type_identity<VkD3D::TypeList<typename decltype(ForkRunTypeHelper<plan.offsets[Rs], plan.lengths[Rs], Passes...>())::type...>> {};
        }(std::make_index_sequence<plan.numRuns> {});
    }
}

}

template <typename... Passes>
using ResourcesOf = typename TemplatedDetail::CollectAllResources<Passes...>::type;

template <typename Resources, typename PassA, typename PassB>
struct ArePassesDisjoint {
    static constexpr auto writes_a = TemplatedDetail::PassFootprint<Resources, PassA>::masks[0];
    static constexpr auto reads_a  = TemplatedDetail::PassFootprint<Resources, PassA>::masks[1];
    static constexpr auto writes_b = TemplatedDetail::PassFootprint<Resources, PassB>::masks[0];
    static constexpr auto reads_b  = TemplatedDetail::PassFootprint<Resources, PassB>::masks[1];

    static constexpr bool value = ((writes_a & (writes_b | reads_b)) == 0) && ((reads_a & writes_b) == 0);
};

template <typename... Passes>
struct AutoFork {
    using type = typename decltype(TemplatedDetail::AutoForkTypeHelper<ResourcesOf<Passes...>, Passes...>())::type;
};

template <typename Resources, typename... Passes>
constexpr auto AutoForkPasses(std::tuple<Passes...> passes) noexcept;

template <typename... Passes>
struct PassPack {
    std::tuple<Passes...> passes;

    constexpr explicit PassPack(Passes&&... p): passes(std::forward<Passes>(p)...) {
    }

    template <typename... OtherPasses>
    constexpr auto operator+(PassPack<OtherPasses...>&& other) && {
        return std::apply(
            [&](auto&&... p1) {
                return std::apply(
                    [&](auto&&... p2) { return PassPack<Passes..., OtherPasses...>(std::forward<decltype(p1)>(p1)..., std::forward<decltype(p2)>(p2)...); },
                    std::move(other.passes)
                );
            },
            std::move(this->passes)
        );
    }

    constexpr auto BuildGraph() &&;
};

template <typename... Passes>
constexpr auto MakePassPack(Passes&&... passes) {
    return PassPack<std::decay_t<Passes>...>(std::forward<Passes>(passes)...);
}

template <typename Tag>
struct ResourceResolver;

template <typename ResourceList>
class ResourceBinder {
  public:
    template <typename ContextImpl>
    constexpr void AutoBind(ContextImpl& impl) noexcept;

    template <typename Image>
    constexpr void Bind(ImageSlice slice) noexcept;

    constexpr auto GetBindings() const noexcept -> std::span<const ImageSlice, ResourceList::size>;

  private:
    std::array<ImageSlice, ResourceList::size> _resources {};
};

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
class RasterPassContext;

template <typename... Passes>
class CompileTimeFrameGraph {
  public:
    using Resources = typename TemplatedDetail::CollectAllResources<Passes...>::type;
    using Binder    = ResourceBinder<Resources>;

    static_assert(
        (VkD3D::FrameGraphPass<Passes> && ...),
        "every graph pass must satisfy VkD3D::FrameGraphPass: inherit VkD3D::RenderPass<\"Name\", Usages...> so the graph can name it and analyse its hazards"
    );

    static constexpr size_t NumPasses    = sizeof...(Passes);
    static constexpr size_t NumResources = Resources::size;

    static constexpr auto StateTable = VkD3D::TemplatedDetail::ComputeStateTable<Resources, Passes...>();

    constexpr explicit CompileTimeFrameGraph(Passes&&... passes);

    template <typename ProfilerT = void, typename DiagnosticsT = void, typename ForkPolicyT = SequentialFork>
    void Execute(
        VkCommandBuffer cmd,
        const Binder&   binder,
        uint32_t        frameIndex  = 0,
        ProfilerT*      profiler    = nullptr,
        DiagnosticsT*   diagnostics = nullptr,
        ForkPolicyT*    forker      = nullptr
    ) const;

  private:
    template <size_t PassIndex, typename PassType>
    struct BarrierPlan {
        using Usages                     = typename PassType::Usages;
        std::array<size_t, Usages::size> indices {};
        size_t                           count = 0;
    };

    template <size_t PassIndex, typename PassType>
    static consteval auto BuildBarrierPlan() noexcept {
        BarrierPlan<PassIndex, PassType> plan {};
        if constexpr (PassType::Usages::size > 0) {
            [&]<size_t... Is>(std::index_sequence<Is...>) {
                (([&] {
                     using UsageType                                     = typename PassType::Usages::template type<Is>;
                     using Img                                           = typename UsageType::Resource;
                     constexpr size_t                         r_idx      = VkD3D::TemplatedDetail::GetResourceIndex<Resources, Img>();
                     constexpr VkD3D::TemplatedDetail::ResourceState prev_state = StateTable[PassIndex][r_idx];
                     if constexpr (VkD3D::TemplatedDetail::NeedsBarrier<prev_state, UsageType, PassIndex>::value) {
                         plan.indices[plan.count++] = Is;
                     }
                 }()),
                 ...);
            }(std::make_index_sequence<PassType::Usages::size> {});
        }
        return plan;
    }

    template <size_t PassIndex, typename PassType, typename ProfilerT, typename DiagnosticsT, typename ForkPolicyT>
    void ExecutePass(
        VkCommandBuffer                                cmd,
        std::span<const ImageSlice, NumResources> bindings,
        const PassType&                                pass,
        uint32_t                                       frameIndex,
        ProfilerT*                                     profiler,
        DiagnosticsT*                                  diagnostics,
        ForkPolicyT*                                   forker
    ) const;

    template <typename ProfilerT>
    static void WriteScopeStart(VkCommandBuffer cmd, uint32_t frameIndex, std::string_view passName, ProfilerT* profiler) noexcept;

    template <typename ProfilerT>
    static void WriteScopeEnd(VkCommandBuffer cmd, uint32_t frameIndex, std::string_view passName, ProfilerT* profiler) noexcept;

    std::tuple<Passes...> _passes;
};

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
class RasterPassContext: public RasterPassContextBase {
  public:
    RasterPassContext(VkCommandBuffer cmd, std::span<const ImageSlice, ResourceList::size> bindings) noexcept;

    ~RasterPassContext() noexcept;

  private:
    std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> m_colors {};

    template <typename... Imgs, typename... DImgs>
    void ResolveExtent(
        std::span<const ImageSlice, ResourceList::size> bindings,
        VkD3D::TypeList<Imgs...> ,
        VkD3D::TypeList<DImgs...>
    ) noexcept;

    template <typename... Imgs>
    void BuildColorAttachments(
        std::span<const ImageSlice, ResourceList::size> bindings,
        uint32_t&                                            colorCount,
        VkD3D::TypeList<Imgs...>
    ) noexcept;

    template <typename... DImgs>
    bool BuildDepthAttachment(
        std::span<const ImageSlice, ResourceList::size> bindings,
        VkRenderingAttachmentInfo&                           outDepth,
        VkD3D::TypeList<DImgs...>
    ) noexcept;
};

template <typename Tag, typename T>
constexpr auto MakeRef(const T& resource) noexcept -> ImageSlice;
template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view) noexcept;
template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view, VkExtent2D extent, Vk::Format format = VkD3D::ToVk(Tag::format)) noexcept;
template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view, VkExtent3D extent, Vk::Format format = VkD3D::ToVk(Tag::format)) noexcept;

}

namespace ZHLN::Vk::Debug {

template <size_t Capacity>
using VisualizerString = VkD3D::TemplatedDetail::ConstexprString<Capacity>;

template <typename T>
struct GraphVisualizer;

template <typename... Passes>
struct GraphVisualizer<CompileTimeFrameGraph<Passes...>> {
    using GraphT                         = CompileTimeFrameGraph<Passes...>;
    using Resources                      = typename GraphT::Resources;
    static constexpr size_t NumPasses    = GraphT::NumPasses;
    static constexpr size_t NumResources = Resources::size;

    static consteval auto Visualize();
};

template <typename GraphT>
struct ForceGraphVisualization {
    static_assert(sizeof(GraphT) == 0, GraphVisualizer<GraphT>::Visualize().string_view());
};

}

#include "RenderGraph.inl"
