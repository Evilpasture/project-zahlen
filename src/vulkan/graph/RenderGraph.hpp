// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Reflection/Enums.hpp>
#include <array>
#include <string_view>

namespace ZHLN::Vk {


template <size_t N>
struct ResourceName {
    std::array<char, N> value {};

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
    constexpr ResourceName(const char (&str)[N]) {
        for (size_t i = 0; i < N; ++i) {
            value[i] = str[i];
        }
    }

    [[nodiscard]] constexpr std::string_view string_view() const noexcept {
        return std::string_view(value.data(), N > 0 && value[N - 1] == '\0' ? N - 1 : N);
    }
};

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
    ResourceName       Name,
    VkFormat           Format,
    VkImageAspectFlags Aspect,
    bool               IsSwapchain  = false,
    bool               IsPersistent = false,
    uint32_t           ScaleDivisor = 1,
    bool               Is3D         = false>
struct GraphImage {
    static constexpr auto               name          = Name;
    static constexpr VkFormat           format        = Format;
    static constexpr VkImageAspectFlags aspect        = Aspect;
    static constexpr bool               is_swapchain  = IsSwapchain;
    static constexpr bool               is_persistent = IsPersistent;
    static constexpr uint32_t           scale_divisor = ScaleDivisor;
    static constexpr bool               is_3d         = Is3D;
};

template <typename Image, VkImageLayout Layout, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access>
struct Usage {
    using Resource                                = Image;
    static constexpr VkImageLayout         layout = Layout;
    static constexpr VkPipelineStageFlags2 stage  = Stage;
    static constexpr VkAccessFlags2        access = Access;
};

template <typename Image>
using ColorWrite = Usage<
    Image,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT>;

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
using ComputeReadGeneral = Usage<Image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using TransferSrcRead = Usage<Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT>;

template <typename Image>
using TransferDstWrite = Usage<Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT>;

template <typename Image>
using ShaderReadGeneral = Usage<Image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <ResourceName Name, typename UsagesList, typename RecordFn_T>
struct GraphPass {
    static constexpr auto name = Name;
    using Usages               = UsagesList;
    using RecordFn             = RecordFn_T;
    RecordFn record;
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


struct ForkBody {
    void* user                                               = nullptr;
    void (*record)(void* user, VkCommandBuffer cmd) noexcept = nullptr;

    void operator()(VkCommandBuffer cmd) const noexcept {
        if (record != nullptr) {
            record(user, cmd);
        }
    }
};

template <typename Executor>
concept ForkRecorder = requires(Executor& executor, VkCommandBuffer cmd, std::span<const ForkBody> bodies) {
    { executor.ExecuteFork(cmd, bodies) } noexcept;
};

struct SequentialFork {
    static constexpr void ExecuteFork(VkCommandBuffer cmd, std::span<const ForkBody> bodies) noexcept {
        for (const ForkBody& body: bodies) {
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

    [[nodiscard]] auto Bodies(std::array<ForkBody, sizeof...(SubPasses)>& out) const noexcept -> std::span<const ForkBody> {
        size_t index = 0;
        std::apply(
            [&](const SubPasses&... p) { ((out[index++] = ForkBody {.user = const_cast<SubPasses*>(&p), .record = &RecordBody<SubPasses>}), ...); }, subPasses
        );
        return {out.data(), out.size()};
    }

  private:
    template <typename SubPass>
    static void RecordBody(void* user, VkCommandBuffer cmd) noexcept {
        static_cast<const SubPass*>(user)->record(cmd);
    }
};

template <typename... SubPasses>
constexpr auto Fork(SubPasses&&... passes) {
    return ParallelPass<std::decay_t<SubPasses>...>(std::forward<SubPasses>(passes)...);
}

namespace TemplatedDetail {

struct BypassGraphicsCheckToken {};

template <typename U>
struct IsColorAttachment: std::false_type {};

template <typename Image, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access>
struct IsColorAttachment<Usage<Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Stage, Access>>: std::true_type {};

template <typename U>
struct IsDepthAttachment: std::false_type {};

template <typename Image, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access>
struct IsDepthAttachment<Usage<Image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, Stage, Access>>: std::true_type {};

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
    static constexpr bool value       = !is_graphics || std::is_invocable_v<typename P::RecordFn, VkCommandBuffer>;
};

template <typename... S>
struct IsForkablePass<ParallelPass<S...>>: std::false_type {};

template <typename List>
struct AllForkablePasses: std::true_type {};

template <typename H, typename... T>
struct AllForkablePasses<TypeList<H, T...>>: std::bool_constant<IsForkablePass<H>::value && AllForkablePasses<TypeList<T...>>::value> {};

template <typename List, typename Candidate>
struct AllDisjointFrom;

template <typename Acc, typename Rest>
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

template <typename List>
struct AutoForkRuns;

template <typename List>
struct FirstRunOfList;

}


template <typename PassA, typename PassB>
struct ArePassesDisjoint {
    using WritesA = TemplatedDetail::Filter<typename PassA::Usages, TemplatedDetail::IsAnyWrite>;
    using ReadsA  = TemplatedDetail::Filter<typename PassA::Usages, TemplatedDetail::IsAnyRead>;
    using WritesB = TemplatedDetail::Filter<typename PassB::Usages, TemplatedDetail::IsAnyWrite>;
    using ReadsB  = TemplatedDetail::Filter<typename PassB::Usages, TemplatedDetail::IsAnyRead>;

    static constexpr bool value = !TemplatedDetail::HasIntersection<WritesA, WritesB>::value && !TemplatedDetail::HasIntersection<WritesA, ReadsB>::value &&
                                  !TemplatedDetail::HasIntersection<ReadsA, WritesB>::value;
};

template <typename... Passes>
struct AutoFork {
    using type = typename TemplatedDetail::AutoForkRuns<TypeList<Passes...>>::type;
};

template <typename... Passes>
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

template <ResourceName Name, typename... Usages, typename RecordFn>
constexpr auto MakePass(RecordFn&& record) {
    constexpr bool has_graphics = (TemplatedDetail::IsColorAttachment<Usages>::value || ...) || (TemplatedDetail::IsDepthAttachment<Usages>::value || ...);

    if constexpr (has_graphics) {
        static_assert(
            !std::is_invocable_v<RecordFn, VkCommandBuffer>, "\n\n================================================================================\n"
                                                             "  [COMPILER ERROR] Render pass safety violation detected!\n"
                                                             "================================================================================\n\n"
                                                             "  Direct use of MakePass with ColorWrite or DepthWrite is not allowed.\n"
                                                             "  Recording draw calls outside of an active Vulkan RenderPass causes undefined "
                                                             "behaviour.\n\n"
                                                             "  Resolution:\n"
                                                             "    - Write your lambdas to accept 'auto& ctx' instead of raw VkCommandBuffer.\n"
                                                             "    - The graph executor will automatically open and close the RenderPass for you.\n\n"
                                                             "================================================================================\n"
        );
    }

    return GraphPass<Name, TypeList<Usages...>, std::decay_t<RecordFn>> {std::forward<RecordFn>(record)};
}

template <ResourceName Name, typename... Usages, typename RecordFn>
constexpr auto Passieren(RecordFn&& record, TemplatedDetail::BypassGraphicsCheckToken  = {}) {
    return GraphPass<Name, TypeList<Usages...>, std::decay_t<RecordFn>> {std::forward<RecordFn>(record)};
}

struct GraphResource {
    VkImage     handle = VK_NULL_HANDLE;
    VkImageView view   = VK_NULL_HANDLE;
    VkExtent3D  extent {};
};

template <typename Tag>
struct ResourceResolver;

template <typename ResourceList>
class ResourceBinder {
  public:
    template <typename ContextImpl>
    constexpr void AutoBind(ContextImpl& impl) noexcept;

    template <typename Image>
    constexpr void Bind(VkImage handle, VkImageView view, VkExtent3D extent) noexcept;

    constexpr auto GetBindings() const noexcept -> const std::array<GraphResource, ResourceList::size>&;

  private:
    std::array<GraphResource, ResourceList::size> _resources {};
};

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
class RasterPassContext;

template <typename... Passes>
class CompileTimeFrameGraph {
  public:
    using Resources = typename TemplatedDetail::CollectAllResources<Passes...>::type;
    using Binder    = ResourceBinder<Resources>;

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
    static consteval size_t CountRequiredBarriers() {
        using Usages = typename PassType::Usages;
        if constexpr (Usages::size == 0) {
            return 0;
        } else {
            return []<size_t... Is>(std::index_sequence<Is...>) {
                size_t count = 0;
                ((count +=
                  []() {
                      using UsageType                                     = typename Usages::template type<Is>;
                      using Img                                           = typename UsageType::Resource;
                      constexpr size_t                         r_idx      = TemplatedDetail::GetResourceIndex<Resources, Img>();
                      constexpr TemplatedDetail::ResourceState prev_state = StateTable[PassIndex][r_idx];
                      return TemplatedDetail::NeedsBarrier<prev_state, UsageType, PassIndex>::value ? 1 : 0;
                  }()),
                 ...);
                return count;
            }(std::make_index_sequence<Usages::size> {});
        }
    }

    template <size_t PassIndex, typename PassType, size_t BarrierCount>
    static consteval std::array<size_t, BarrierCount> GetBarrierUsageIndices() {
        std::array<size_t, BarrierCount> indices {};
        using Usages = typename PassType::Usages;

        if constexpr (Usages::size == 0) {
            return indices;
        } else {
            [&]<size_t... Is>(std::index_sequence<Is...>) {
                size_t write_idx = 0;
                (([&]() {
                     using UsageType                                     = typename Usages::template type<Is>;
                     using Img                                           = typename UsageType::Resource;
                     constexpr size_t                         r_idx      = TemplatedDetail::GetResourceIndex<Resources, Img>();
                     constexpr TemplatedDetail::ResourceState prev_state = StateTable[PassIndex][r_idx];
                     if constexpr (TemplatedDetail::NeedsBarrier<prev_state, UsageType, PassIndex>::value) {
                         indices[write_idx++] = Is;
                     }
                 }()),
                 ...);
            }(std::make_index_sequence<Usages::size> {});

            return indices;
        }
    }

    template <size_t PassIndex, typename PassType, typename ProfilerT, typename DiagnosticsT, typename ForkPolicyT>
    void ExecutePass(
        VkCommandBuffer                                cmd,
        const std::array<GraphResource, NumResources>& bindings,
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
class RasterPassContext {
  public:
    RasterPassContext(VkCommandBuffer cmd, const std::array<GraphResource, ResourceList::size>& bindings) noexcept;

    ~RasterPassContext() noexcept;

    [[nodiscard]] VkCommandBuffer Cmd() const noexcept;
    [[nodiscard]] VkExtent2D      Extent() const noexcept;

  private:
    VkCommandBuffer                                             m_cmd;
    VkExtent2D                                                  m_extent {};
    std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> m_colors {};

    template <typename... Imgs, typename... DImgs>
    void ResolveExtent(
        const std::array<GraphResource, ResourceList::size>& bindings,
        TypeList<Imgs...> ,
        TypeList<DImgs...>
    ) noexcept;

    template <typename... Imgs>
    void BuildColorAttachments(
        const std::array<GraphResource, ResourceList::size>& bindings,
        uint32_t&                                            colorCount,
        TypeList<Imgs...>
    ) noexcept;

    template <typename... DImgs>
    bool BuildDepthAttachment(
        const std::array<GraphResource, ResourceList::size>& bindings,
        VkRenderingAttachmentInfo&                           outDepth,
        TypeList<DImgs...>
    ) noexcept;
};

template <typename Tag>
struct GraphImageRef {
    using TagType      = Tag;
    VkImage     handle = VK_NULL_HANDLE;
    VkImageView view   = VK_NULL_HANDLE;
    VkExtent3D  extent {};
};

template <typename Tag, typename T>
constexpr auto MakeRef(const T& resource) noexcept;
template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view) noexcept;
template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view, VkExtent2D extent) noexcept;

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
