// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "GraphImage.hpp"
#include <Zahlen/Core/FunctionRef.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <array>
#include <span>
#include <string_view>

namespace ZHLN::Vk {

template <typename... Ts>
struct TypeList {
    static constexpr size_t size = sizeof...(Ts);

    template <size_t I>
    using type = Ts...[I];
};

template <typename List, typename Target>
struct IsInList;
template <typename... Ts, typename Target>
struct IsInList<Vk::TypeList<Ts...>, Target> {
    static constexpr bool value = (std::is_same_v<Ts, Target> || ...);
};

namespace detail {
struct DummyResource {
    static constexpr auto name = ResourceName("DummyResource");
};
struct DummyUsage {
    using Resource                                = DummyResource;
    static constexpr VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
    static constexpr VkPipelineStageFlags2 stage  = VK_PIPELINE_STAGE_2_NONE;
    static constexpr VkAccessFlags2        access = 0;
};
}

template <>
struct TypeList<> {
    static constexpr size_t size = 0;

    template <size_t I>
    using type = detail::DummyUsage;
};

template <
    typename Image, VkImageLayout Layout, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access,
    VkImageLayout FinalLayout = Layout, VkPipelineStageFlags2 FinalStage = Stage, VkAccessFlags2 FinalAccess = Access,
    VkColorComponentFlags ChannelMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT>
struct Usage {
    using Resource                                      = Image;
    static constexpr VkImageLayout         layout       = Layout;
    static constexpr VkPipelineStageFlags2 stage        = Stage;
    static constexpr VkAccessFlags2        access       = Access;
    static constexpr VkImageLayout         final_layout = FinalLayout;
    static constexpr VkPipelineStageFlags2 final_stage  = FinalStage;
    static constexpr VkAccessFlags2        final_access = FinalAccess;
    static constexpr VkColorComponentFlags channel_mask = ChannelMask;
};

template <typename Image>
using ColorWrite = Usage<
    Image,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT>;

// Full Vulkan image synchronization is still required: this mask only
// constrains the pipeline's color writes, not barriers or shader declarations.
template <typename Image>
using ColorWriteRGB = Usage<
    Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT>;

template <typename Image>
using ShaderRead = Usage<Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using DepthWrite = Usage<
    Image,
    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT>;

template <typename Image>
using DepthStencilWrite = Usage<
    Image,
    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT>;

template <typename Image>
using ComputeWrite = Usage<Image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using ComputeRead = Usage<Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using FragmentAndComputeRead = Usage<
    Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using ComputeReadGeneral = Usage<Image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using TransferSrcRead = Usage<Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT>;

template <typename Image>
using TransferDstWrite = Usage<Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT>;

template <typename Image>
using TransferDstWriteThenShaderRead = Usage<
    Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using ShaderReadGeneral = Usage<Image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename T>
concept FrameGraphPass = requires {
    { T::name.string_view() } -> std::convertible_to<std::string_view>;
    typename T::Usages;
};

template <ResourceName Name, typename... UsagesList>
struct RenderPass {
    static constexpr auto name = Name;
    using Usages               = TypeList<UsagesList...>;
};

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

using ForkCall = ZHLN::FunctionRef<void(VkCommandBuffer) const>;

template <typename Executor>
concept ForkRecorder = requires(Executor& executor, VkCommandBuffer cmd, std::span<const ForkCall> bodies) {
    { executor.ExecuteFork(cmd, bodies) } noexcept;
};

struct SequentialFork {
    static constexpr void ExecuteFork(VkCommandBuffer cmd, std::span<const ForkCall> bodies) noexcept {
        for (const ForkCall& body: bodies) {
            body(cmd);
        }
    }
};

static_assert(ForkRecorder<SequentialFork>);

template <typename... SubPasses>
struct ParallelPass {
    static constexpr auto name = ResourceName("ParallelPassGroup");
    using Usages = typename TemplatedDetail::MergeFold<TypeList<>, typename SubPasses::Usages...>::type;

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

template <typename U>
struct IsColorAttachment: std::false_type {};

template <
    typename Image, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access,
    VkImageLayout FinalLayout, VkPipelineStageFlags2 FinalStage, VkAccessFlags2 FinalAccess, VkColorComponentFlags Mask>
struct IsColorAttachment<Usage<Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Stage, Access, FinalLayout, FinalStage, FinalAccess, Mask>>: std::true_type {};

template <typename U>
struct IsDepthAttachment: std::false_type {};

template <
    typename Image, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access,
    VkImageLayout FinalLayout, VkPipelineStageFlags2 FinalStage, VkAccessFlags2 FinalAccess, VkColorComponentFlags Mask>
struct IsDepthAttachment<Usage<Image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, Stage, Access, FinalLayout, FinalStage, FinalAccess, Mask>>: std::true_type {};

template <typename InList, typename OutList, template <typename> class Predicate>
struct FilterImpl;

template <typename List, template <typename> class Predicate>
using Filter = typename FilterImpl<List, TypeList<>, Predicate>::type;

template <typename... Passes>
struct CollectAllResources;

template <typename Target, typename... Ts>
consteval auto GetResourceIndexImpl(TypeList<Ts...> ) -> size_t;

template <size_t Capacity>
struct ConstexprString {
    std::array<char, Capacity> dataBuffer {};
    size_t                     length = 0;

    constexpr void append(std::string_view sv) noexcept {
        const size_t to_copy = sv.size() < (Capacity - 1 - length) ? sv.size() : (Capacity - 1 - length);
        for (size_t i = 0; i < to_copy; ++i) {
            dataBuffer[length + i] = sv[i];
        }
        length += to_copy;
        dataBuffer[length] = '\0';
    }

    constexpr void append_int(size_t val) noexcept {
        if (val == 0) {
            append("0");
            return;
        }
        std::array<char, 24> temp {};
        size_t               i = 0;
        while (val > 0 && i < 23) {
            temp[i++] = static_cast<char>('0' + (val % 10));
            val /= 10;
        }
        for (size_t j = 0; j < i / 2; ++j) {
            const char c    = temp[j];
            temp[j]         = temp[i - 1 - j];
            temp[i - 1 - j] = c;
        }
        append(std::string_view(temp.data(), i));
    }

    template <typename E>
        requires std::is_enum_v<E>
    constexpr void append_enum(E value) noexcept {
        using Under     = std::underlying_type_t<E>;
        const auto bits = static_cast<Under>(value);
        bool       any  = false;
        Reflect::ForEachEnumerator<E>([&]<E Val>() {
            const auto v = static_cast<Under>(Val);
            if (v != 0 && (bits & v) == v) {
                if (any) {
                    append(" | ");
                }
                any = true;
                append(Reflect::EnumToString(Val));
            }
        });
        if (!any) {
            append(Reflect::EnumToString(value));
        }
    }

    [[nodiscard]] constexpr auto string_view() const noexcept -> std::string_view {
        return std::string_view(dataBuffer.data(), length);
    }
};

template <typename ResourceList, typename Target>
consteval auto GetResourceIndex() -> size_t;

struct ResourceState {
    VkImageLayout         layout            = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage             = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        access            = 0;
    size_t                lastWritePass     = 999999;
    bool                  fromPreviousFrame = false;
};

constexpr VkAccessFlags2 WriteMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                     VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

template <typename U>
struct IsAnyWrite: std::bool_constant<(U::access & WriteMask) != 0> {};

template <typename U>
struct IsAnyRead: std::bool_constant<(U::access & WriteMask) == 0> {};

template <typename ListA, typename ListB>
struct HasIntersection: std::false_type {};

template <typename... As, typename... Bs>
struct HasIntersection<TypeList<As...>, TypeList<Bs...>>: std::bool_constant<(IsInList<TypeList<Bs...>, As>::value || ...)> {};

template <typename Resources, typename Pass>
struct PassFootprint {
    static constexpr size_t kMaxResources = 64;

    template <typename Usages, size_t I>
    static constexpr auto Fold(std::array<uint64_t, 2> accumulated) noexcept {
        using Usage            = typename Usages::template type<I>;
        constexpr size_t index = GetResourceIndex<Resources, typename Usage::Resource>();
        static_assert(index < kMaxResources, "The frame graph's hazard masks hold 64 resources; widen them before the graph reaches 65.");
        const uint64_t bit = uint64_t {1} << index;
        accumulated[(Usage::access & WriteMask) != 0 ? 0 : 1] |= bit;
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

template <ResourceState Prev, typename Usage, size_t PassIndex>
struct NeedsBarrier {
    static constexpr bool is_prev_write = (Prev.access & WriteMask) != 0;
    static constexpr bool is_curr_write = (Usage::access & WriteMask) != 0;

    static constexpr bool value = (is_curr_write && (Prev.fromPreviousFrame || (Prev.lastWritePass < PassIndex))) ||
                                  (is_prev_write && !is_curr_write && (Prev.lastWritePass < PassIndex)) || (Prev.layout != Usage::layout) ||
                                  (Prev.layout == VK_IMAGE_LAYOUT_UNDEFINED);
};

template <typename ResourceList, typename... Passes>
consteval auto ComputeStateTable();

template <typename P>
struct IsForkablePass {
    using Usages      = typename P::Usages;
    using ColorWrites = Filter<Usages, IsColorAttachment>;
    using DepthWrites = Filter<Usages, IsDepthAttachment>;

    static constexpr bool is_graphics = (ColorWrites::size > 0) || (DepthWrites::size > 0);
    static constexpr bool value       = !is_graphics || std::is_invocable_v<P, VkCommandBuffer>;
};

template <typename... S>
struct IsForkablePass<ParallelPass<S...>>: std::false_type {};

template <typename List>
struct AllForkablePasses: std::true_type {};

template <typename H, typename... T>
struct AllForkablePasses<TypeList<H, T...>>: std::bool_constant<IsForkablePass<H>::value && AllForkablePasses<TypeList<T...>>::value> {};

template <typename Resources, typename List, typename Candidate>
struct AllDisjointFrom;

template <typename Resources, typename Acc, typename Rest>
struct FirstRun;

template <typename List, size_t N>
struct DropFront;

template <typename List, typename T>
struct Cons {
    using type = TypeList<>;
};

template <typename... Ts, typename T>
struct Cons<TypeList<Ts...>, T> {
    using type = TypeList<Ts..., T>;
};

template <typename A, typename B>
struct AppendLists {
    using type = TypeList<>;
};

template <typename... A, typename... B>
struct AppendLists<TypeList<A...>, TypeList<B...>> {
    using type = TypeList<A..., B...>;
};

template <typename Run>
struct WrapRun;

template <typename P>
struct WrapRun<TypeList<P>> {
    using type = P;
};

template <typename A, typename... B>
struct WrapRun<TypeList<A, B...>> {
    using type = ParallelPass<A, B...>;
};

template <typename Resources, typename List>
struct AutoForkRuns;

template <typename Resources, typename List>
struct FirstRunOfList;

}

// Public graph contract for pipeline setup. Keep internal traversal details in
// TemplatedDetail so render code never depends on its implementation namespace.
template <typename Pass, typename Image>
consteval VkColorComponentFlags ColorAttachmentWriteMask() {
    return []<typename... Us>(TypeList<Us...>) consteval -> VkColorComponentFlags {
        constexpr size_t matches = (size_t(std::is_same_v<typename Us::Resource, Image> && TemplatedDetail::IsColorAttachment<Us>::value) + ... + 0u);
        static_assert(matches == 1u, "Pass must declare exactly one color write for this attachment");
        return ((std::is_same_v<typename Us::Resource, Image> && TemplatedDetail::IsColorAttachment<Us>::value ? Us::channel_mask : 0u) | ... | 0u);
    }(typename Pass::Usages {});
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
    using type = typename TemplatedDetail::AutoForkRuns<ResourcesOf<Passes...>, TypeList<Passes...>>::type;
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

class RasterPassContextBase {
  public:
    explicit RasterPassContextBase(VkCommandBuffer cmd) noexcept: m_cmd(cmd) {
    }
    ~RasterPassContextBase() = default;

    RasterPassContextBase(const RasterPassContextBase&)                = delete;
    auto operator=(const RasterPassContextBase&) -> RasterPassContextBase& = delete;
    RasterPassContextBase(RasterPassContextBase&&) noexcept            = delete;
    auto operator=(RasterPassContextBase&&) noexcept -> RasterPassContextBase& = delete;

    [[nodiscard]] auto Cmd() const noexcept -> VkCommandBuffer;
    [[nodiscard]] auto Extent() const noexcept -> VkExtent2D;

  protected:
    void SetExtent(VkExtent2D extent) noexcept;

  private:
    VkCommandBuffer m_cmd;
    VkExtent2D      m_extent {};
};

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
class RasterPassContext;

template <typename... Passes>
class CompileTimeFrameGraph {
  public:
    using Resources = typename TemplatedDetail::CollectAllResources<Passes...>::type;
    using Binder    = ResourceBinder<Resources>;

    static_assert(
        (FrameGraphPass<Passes> && ...),
        "every graph pass must satisfy Vk::FrameGraphPass: inherit Vk::RenderPass<\"Name\", Usages...> so the graph can name it and analyse its hazards"
    );

    static constexpr size_t NumPasses    = sizeof...(Passes);
    static constexpr size_t NumResources = Resources::size;

    static constexpr auto StateTable = TemplatedDetail::ComputeStateTable<Resources, Passes...>();

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
                     constexpr size_t                         r_idx      = TemplatedDetail::GetResourceIndex<Resources, Img>();
                     constexpr TemplatedDetail::ResourceState prev_state = StateTable[PassIndex][r_idx];
                     if constexpr (TemplatedDetail::NeedsBarrier<prev_state, UsageType, PassIndex>::value) {
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

template <typename Tag>
struct ClearColorOf {
    static constexpr Color4 value = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
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
        TypeList<Imgs...> ,
        TypeList<DImgs...>
    ) noexcept;

    template <typename... Imgs>
    void BuildColorAttachments(
        std::span<const ImageSlice, ResourceList::size> bindings,
        uint32_t&                                            colorCount,
        TypeList<Imgs...>
    ) noexcept;

    template <typename... DImgs>
    bool BuildDepthAttachment(
        std::span<const ImageSlice, ResourceList::size> bindings,
        VkRenderingAttachmentInfo&                           outDepth,
        TypeList<DImgs...>
    ) noexcept;
};

template <typename Tag, typename T>
constexpr auto MakeRef(const T& resource) noexcept -> ImageSlice;
template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view) noexcept;
template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view, VkExtent2D extent, VkFormat format = Tag::format) noexcept;
template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view, VkExtent3D extent, VkFormat format = Tag::format) noexcept;

}

namespace ZHLN::Vk::Debug {

template <size_t Capacity>
using VisualizerString = TemplatedDetail::ConstexprString<Capacity>;

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
