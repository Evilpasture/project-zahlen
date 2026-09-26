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

// Compile-Time Resource Identification & Tagging

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

// Defined early so GraphPass and MakePass can resolve it during compilation
template <typename... Ts>
struct TypeList {
    static constexpr size_t size = sizeof...(Ts);

    // C++26 Pack Indexing on Types: T...[N]
    template <size_t I>
    using type = Ts...[I];
};

// Compile-time frame graph membership predicate
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
// Dummy usage stub for zero-usage passes (e.g. BDA compute passes)
struct DummyUsage {
    using Resource                                = DummyResource;
    static constexpr VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
    static constexpr VkPipelineStageFlags2 stage  = VK_PIPELINE_STAGE_2_NONE;
    static constexpr VkAccessFlags2        access = 0;
};
} // namespace detail

template <>
struct TypeList<> {
    static constexpr size_t size = 0;

    template <size_t I>
    using type = detail::DummyUsage; // Safe fallback struct instead of void
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

// Left fold of MergeLists, so a pass group can name any number of usage lists.
template <typename Accumulated, typename... Lists>
struct MergeFold;

// Always false, but only once the enclosing template is instantiated: the
// idiom every "this overload requires the caller to do X" diagnostic here
// uses, so a static_assert in a branch stays dependent.
template <typename...>
inline constexpr bool DependentFalse = false;

} // namespace TemplatedDetail

// Vk::Fork -- compile-time parallel pass group
//
// A Fork is a group of passes touching *disjoint* resources, recorded concurrently on worker
// threads and replayed into the main stream with vkCmdExecuteCommands. Each sub-pass keeps its
// own honest usage list and the group exposes the compile-time union, so the graph emits every
// barrier the group needs before any of it is recorded.
//
// The graph deliberately does not know how to record in parallel -- threading is an engine
// service. It hands the sub-pass bodies to the executor passed to
// `CompileTimeFrameGraph::Execute`, a template parameter rather than a virtual interface, so
// the call resolves statically and the header stays header-only. No executor means
// `SequentialFork`: same barriers, same resources, recorded in stream order on the calling
// thread.

// Type-erased body of one forked sub-pass.
struct ForkBody {
    void* user                                               = nullptr;
    void (*record)(void* user, VkCommandBuffer cmd) noexcept = nullptr;

    void operator()(VkCommandBuffer cmd) const noexcept {
        if (record != nullptr) {
            record(user, cmd);
        }
    }
};

// What a fork executor has to offer: one call that records every body of the
// group and replays them into `cmd`. Barrier and layout work for the whole
// group is already recorded by the time this is called, so an executor that
// records into secondaries must inherit the primary's descriptor-heap state
// rather than rebind it.
template <typename Executor>
concept ForkRecorder = requires(Executor& executor, VkCommandBuffer cmd, std::span<const ForkBody> bodies) {
    { executor.ExecuteFork(cmd, bodies) } noexcept;
};

// The executor a graph runs with when it is given none.
//
// A policy rather than a fallback: it records the group's bodies in
// declaration order into the frame's own command buffer, which is exactly
// what a parallel executor replays after recording the same bodies into
// secondaries -- same barriers, same resources, no threads.
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
    // Compile-time union of every resource the sub-passes touch. MergeLists
    // de-duplicates by resource type, which is what makes one barrier per
    // resource legal for the whole group.
    using Usages = typename TemplatedDetail::MergeFold<TypeList<>, typename SubPasses::Usages...>::type;

    static constexpr bool   is_fork    = true;
    static constexpr size_t kBodyCount = sizeof...(SubPasses);

    std::tuple<SubPasses...> subPasses;

    constexpr explicit ParallelPass(SubPasses&&... passes) noexcept: subPasses(std::forward<SubPasses>(passes)...) {
    }

    // Bodies in sub-pass order, for the executor and for the sequential
    // fallback. Storage is the pass's own tuple, so the pointers stay valid
    // for the whole of Execute().
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

// Bypass token for authorized framework-level render pass builders
struct BypassGraphicsCheckToken {};

// Type traits to identify rasterization/attachment writes
template <typename U>
struct IsColorAttachment: std::false_type {};

template <typename Image, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access>
struct IsColorAttachment<Usage<Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Stage, Access>>: std::true_type {};

template <typename U>
struct IsDepthAttachment: std::false_type {};

template <typename Image, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access>
struct IsDepthAttachment<Usage<Image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, Stage, Access>>: std::true_type {};

// Metaprogramming list filter
template <typename InList, typename OutList, template <typename> class Predicate>
struct FilterImpl;

template <typename List, template <typename> class Predicate>
using Filter = typename FilterImpl<List, TypeList<>, Predicate>::type;

template <typename... Passes>
struct CollectAllResources;

template <typename Target, typename... Ts>
consteval auto GetResourceIndexImpl(TypeList<Ts...> /*unused*/) -> size_t;

// Zero-allocation compile-time string. Graph error messages and the
// visualizer share this buffer; append_enum names BarrierStage / BarrierAccess
// through Reflect::EnumToString rather than a hand-written switch.
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

// The predicate pair for compile-time hazard checking: a usage is a write if
// it carries any of the bits NeedsBarrier counts as a write, otherwise a
// read. The checker and the barrier emitter share `WriteMask`, so they cannot
// disagree about what a usage does to a resource.
template <typename U>
struct IsAnyWrite: std::bool_constant<(U::access & WriteMask) != 0> {};

template <typename U>
struct IsAnyRead: std::bool_constant<(U::access & WriteMask) == 0> {};

// True if two TypeLists name at least one type in common. Used over the
// resource lists produced by `Filter`, so "W(A) intersects W(B)" means "some
// resource is written by both passes".
template <typename ListA, typename ListB>
struct HasIntersection: std::false_type {};

template <typename... As, typename... Bs>
struct HasIntersection<TypeList<As...>, TypeList<Bs...>>: std::bool_constant<(IsInList<TypeList<Bs...>, As>::value || ...)> {};

template <ResourceState Prev, typename Usage, size_t PassIndex>
struct NeedsBarrier {
    static constexpr bool is_prev_write = (Prev.access & WriteMask) != 0;
    static constexpr bool is_curr_write = (Usage::access & WriteMask) != 0;

    // An active barrier is only needed if:
    // 1. We are writing, and we have a WAW/RAW hazard (either from this frame or carried over)
    // 2. Or, we are reading, but the previous state was a write (RAW hazard)
    // 3. Or, the layout changed (even if both are reads, e.g., GENERAL to SHADER_READ_ONLY)
    // 4. Or, the resource starts uninitialized (UNDEFINED)
    static constexpr bool value = (is_curr_write && (Prev.fromPreviousFrame || (Prev.lastWritePass < PassIndex))) ||
                                  (is_prev_write && !is_curr_write && (Prev.lastWritePass < PassIndex)) || (Prev.layout != Usage::layout) ||
                                  (Prev.layout == VK_IMAGE_LAYOUT_UNDEFINED);
};

template <typename ResourceList, typename... Passes>
consteval auto ComputeStateTable();

// ---- Automatic fork partition
//
// The type-level side of `AutoForkPasses`: it walks a flat pass list and groups it into
// maximal contiguous *runs* the fork executor may record concurrently. A run grows greedily
// left to right -- a candidate joins only while it and every member are forkable
// (`IsForkablePass`) and it is hazard-free against all of them; earlier members were
// pairwise-checked when they joined, so the invariant holds by induction.

// A pass may join an auto-forked run only if the executor can run its body against a bare
// command buffer, which is what a fork body does. The rule mirrors `ExecutePass`'s leaf
// branch: non-graphics passes always record into the raw buffer; a graphics pass is forkable
// only if its record function takes a `VkCommandBuffer` (the `Passieren` style, managing its
// own render pass). `MakePass`-style bodies take the executor's `RasterPassContext` and so
// stay singleton runs, as does a manual `Vk::Fork` group, whose type-erased callbacks neither
// join a run nor split across runs.
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

// Every element of `List` can run as a fork body (see `IsForkablePass`).
template <typename List>
struct AllForkablePasses: std::true_type {};

template <typename H, typename... T>
struct AllForkablePasses<TypeList<H, T...>>: std::bool_constant<IsForkablePass<H>::value && AllForkablePasses<TypeList<T...>>::value> {};

// Every element of `List` is hazard-free against the single `Candidate`.
template <typename List, typename Candidate>
struct AllDisjointFrom;

// The maximal run that starts at the first element of `Rest`: the longest
// prefix of `Rest` in which each element joins the run built so far.
template <typename Acc, typename Rest>
struct FirstRun;

// Drop the first `N` elements of a TypeList.
template <typename List, size_t N>
struct DropFront;

// `List` plus `T` appended, without de-duplication.
template <typename List, typename T>
struct Cons {
    using type = TypeList<>;
};

template <typename... Ts, typename T>
struct Cons<TypeList<Ts...>, T> {
    using type = TypeList<Ts..., T>;
};

// `A` followed by `B`, without de-duplication.
template <typename A, typename B>
struct AppendLists {
    using type = TypeList<>;
};

template <typename... A, typename... B>
struct AppendLists<TypeList<A...>, TypeList<B...>> {
    using type = TypeList<A..., B...>;
};

// The graph type one run builds: a single pass stays itself, a run of two or
// more becomes one `ParallelPass` over exactly those members.
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

// The whole partition of a pass list: `TypeList<Run1, Run2, ...>` where each
// `RunI` is a `TypeList` of the passes in that run, in original order.
template <typename List>
struct AutoForkRuns;

// The first run of `List` -- `FirstRun` needs a head and a tail, so this is
// the shape that hands it both.
template <typename List>
struct FirstRunOfList;

} // namespace TemplatedDetail

// Compile-Time Hazard Checking & Automatic Forking
//
// The ECS system graph's conflict check applied to pass usage lists: two passes may run
// concurrently iff they never touch the same resource with at least one write; shared reads
// are not a hazard. The declared usage lists are the single source of truth -- a pass writing
// through a raw device address without declaring it is invisible here, the same honesty
// invariant a hand-written Vk::Fork relies on.

template <typename PassA, typename PassB>
struct ArePassesDisjoint {
    using WritesA = TemplatedDetail::Filter<typename PassA::Usages, TemplatedDetail::IsAnyWrite>;
    using ReadsA  = TemplatedDetail::Filter<typename PassA::Usages, TemplatedDetail::IsAnyRead>;
    using WritesB = TemplatedDetail::Filter<typename PassB::Usages, TemplatedDetail::IsAnyWrite>;
    using ReadsB  = TemplatedDetail::Filter<typename PassB::Usages, TemplatedDetail::IsAnyRead>;

    static constexpr bool value = !TemplatedDetail::HasIntersection<WritesA, WritesB>::value && !TemplatedDetail::HasIntersection<WritesA, ReadsB>::value &&
                                  !TemplatedDetail::HasIntersection<ReadsA, WritesB>::value;
};

// The pass pack a frame graph should be built with: every maximal contiguous run of
// pairwise hazard-free forkable passes becomes one `ParallelPass`, recorded through the fork
// executor without a hand-written `Vk::Fork`. Manual fork groups and `MakePass` passes are
// atomic single-element runs. Building from `type` is barrier-equivalent to the original
// order: a `ParallelPass` exposes the union of its members' usages (as the state table
// already relies on for hand-written forks) and pass order inside a run is preserved.
template <typename... Passes>
struct AutoFork {
    using type = typename TemplatedDetail::AutoForkRuns<TypeList<Passes...>>::type;
};

// The runtime twin of `AutoFork`: wraps the given flat pass tuple according
// to the compile-time partition and returns the tuple the graph should be
// built from. Element order is preserved; each element is either the pass
// unchanged (run of one) or the `ParallelPass` over its run.
template <typename... Passes>
constexpr auto AutoForkPasses(std::tuple<Passes...> passes) noexcept;

// A concatenatable group of passes for building a frame graph. Packs hold
// their passes by value and are joined with `+` at compile time;
// `BuildGraph` then partitions the concatenated flat list (see
// `AutoForkPasses`) into fork bundles and constructs the graph -- replacing
// hand-rolled `std::tuple_cat` / `std::apply` plumbing at the call site.
template <typename... Passes>
struct PassPack {
    std::tuple<Passes...> passes;

    constexpr explicit PassPack(Passes&&... p): passes(std::forward<Passes>(p)...) {
    }

    // Join two packs into one flat pack; the element order is preserved.
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

    // The fork partition of this pack's flat list, compiled into a frame
    // graph: every maximal run of hazard-free forkable passes becomes one
    // `ParallelPass`, exactly as for a hand-built pass tuple.
    constexpr auto BuildGraph() &&;
};

// A `PassPack` over the decayed types of the given pass values.
template <typename... Passes>
constexpr auto MakePassPack(Passes&&... passes) {
    return PassPack<std::decay_t<Passes>...>(std::forward<Passes>(passes)...);
}

/**
 * @brief SAFE, compile-time verified pass builder.
 * Triggers a static assertion if rasterization attachments are used directly.
 */
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

/**
 * @brief UNSAFE / Framework-only pass builder.
 * Required for internal wrappers that manually handle render pass boundaries.
 */
template <ResourceName Name, typename... Usages, typename RecordFn>
constexpr auto Passieren(RecordFn&& record, TemplatedDetail::BypassGraphicsCheckToken /*unused*/ = {}) {
    return GraphPass<Name, TypeList<Usages...>, std::decay_t<RecordFn>> {std::forward<RecordFn>(record)};
}

struct GraphResource {
    VkImage     handle = VK_NULL_HANDLE;
    VkImageView view   = VK_NULL_HANDLE;
    VkExtent3D  extent {}; // Upgraded to 3D to support volumetric targets
};

// Compile-time binding source for one resource tag. A tag that specializes this trait is
// *not* resolved from the reflected `GraphResources` bundle -- the specialization supplies its
// own accessor, because the value is frame-level state (the presentation depth target, the
// ping-ponged accumulation pair, the swapchain image). Tags without one must be reflected
// members of `GraphResources`, which `ResourceBinder::AutoBind` finds through the metadata.
template <typename Tag>
struct ResourceResolver;

template <typename ResourceList>
class ResourceBinder {
  public:
    // Bind every tag of the compiled graph from `impl`: from a
    // `ResourceResolver<Tag>` specialization when one exists, otherwise from
    // the reflected `impl.graphResources` bundle (located by matching the
    // tag against the bundle's `ReflectMetadata`).
    template <typename ContextImpl>
    constexpr void AutoBind(ContextImpl& impl) noexcept;

    template <typename Image>
    // Upgraded parameter signature to 3D
    constexpr void Bind(VkImage handle, VkImageView view, VkExtent3D extent) noexcept;

    constexpr auto GetBindings() const noexcept -> const std::array<GraphResource, ResourceList::size>&;

  private:
    std::array<GraphResource, ResourceList::size> _resources {};
};

// Forward declare RasterPassContext to satisfy CompileTimeFrameGraph dependencies
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

    /**
     * Record every pass, injecting optional diagnostics and profiling. A diagnostics backend
     * receives the compile-time pass name before barriers are recorded; a profiler maps that
     * name to its reflected StageType enum, and passes without a matching enumerator are left
     * unprofiled. `forker` is the parallel-recording service, if any -- a caller without one
     * passes nothing and the bodies record in stream order on this thread.
     */
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
            return 0; // Short-circuit passes with zero image usages
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
            return indices; // Short-circuit passes with zero image usages
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

    // Writes the start (or the end) timestamp of one named scope. Factored out
    // because a forked group brackets `ExecuteFork` with every sub-pass name.
    template <typename ProfilerT>
    static void WriteScopeStart(VkCommandBuffer cmd, uint32_t frameIndex, std::string_view passName, ProfilerT* profiler) noexcept;

    template <typename ProfilerT>
    static void WriteScopeEnd(VkCommandBuffer cmd, uint32_t frameIndex, std::string_view passName, ProfilerT* profiler) noexcept;

    std::tuple<Passes...> _passes;
};

// Automatic RenderPass Execution Context

template <typename Tag>
struct ClearColorOf {
    static constexpr Color4 value = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F}; // Default: Black
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

    // The write lists arrive unfolded into their resource packs: the bodies
    // fold over the types directly, no index sequence to thread through.
    template <typename... Imgs, typename... DImgs>
    void ResolveExtent(
        const std::array<GraphResource, ResourceList::size>& bindings,
        TypeList<Imgs...> /*unused*/,
        TypeList<DImgs...> /*unused*/
    ) noexcept;

    template <typename... Imgs>
    void BuildColorAttachments(
        const std::array<GraphResource, ResourceList::size>& bindings,
        uint32_t&                                            colorCount,
        TypeList<Imgs...> /*unused*/
    ) noexcept;

    template <typename... DImgs>
    bool BuildDepthAttachment(
        const std::array<GraphResource, ResourceList::size>& bindings,
        VkRenderingAttachmentInfo&                           outDepth,
        TypeList<DImgs...> /*unused*/
    ) noexcept;
};

/**
 * @brief Pack compile-time resource tags alongside runtime Vulkan handles.
 */
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

} // namespace ZHLN::Vk

// Debug Tools & Compile-Time Inspection API

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

} // namespace ZHLN::Vk::Debug

#include "RenderGraph.inl"
