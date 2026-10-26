// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderGraph.hpp"

namespace ZHLN::Vk {

namespace TemplatedDetail {

template <typename UsagesList, typename TargetResource>
struct FindResourceUsage {
    using type = void;
};

template <typename Head, typename... Tail, typename TargetResource>
struct FindResourceUsage<TypeList<Head, Tail...>, TargetResource> {
    static consteval auto Select() {
        if constexpr (std::is_same_v<typename Head::Resource, TargetResource>) {
            return std::type_identity<Head> {};
        } else {
            return std::type_identity<typename FindResourceUsage<TypeList<Tail...>, TargetResource>::type> {};
        }
    }
    using type = typename decltype(Select())::type;
};

template <typename OutList, template <typename> class Predicate>
struct FilterImpl<TypeList<>, OutList, Predicate> {
    using type = OutList;
};

template <typename Head, typename... Tail, typename... Out, template <typename> class Predicate>
struct FilterImpl<TypeList<Head, Tail...>, TypeList<Out...>, Predicate> {
    using type = typename std::conditional_t<
        Predicate<Head>::value,
        FilterImpl<TypeList<Tail...>, TypeList<Out..., typename Head::Resource>, Predicate>,
        FilterImpl<TypeList<Tail...>, TypeList<Out...>, Predicate>>::type;
};

template <typename Accumulated>
struct MergeFold<Accumulated> {
    using type = Accumulated;
};

template <typename Accumulated, typename Head, typename... Tail>
struct MergeFold<Accumulated, Head, Tail...> {
    using type = typename MergeFold<typename MergeLists<Accumulated, Head>::type, Tail...>::type;
};

template <typename... Ts, typename T>
struct AppendUnique<TypeList<Ts...>, T> {
    static constexpr bool contains = (std::is_same_v<Ts, T> || ...);
    using type                     = std::conditional_t<contains, TypeList<Ts...>, TypeList<Ts..., T>>;
};

template <typename Existing, typename Incoming>
struct CombineSameResourceUsage {
    using type = Existing;
};

template <typename Existing, typename Incoming>
    requires std::is_same_v<typename Existing::Resource, typename Incoming::Resource>
struct CombineSameResourceUsage<Existing, Incoming> {
    static_assert(
        Existing::layout == Incoming::layout && Existing::final_layout == Incoming::final_layout,
        "Concurrent passes in a ParallelPass cannot require different layouts for the same resource."
    );
    using type = Usage<
        typename Existing::Resource,
        Existing::layout,
        Existing::stage | Incoming::stage,
        Existing::access | Incoming::access,
        Existing::final_layout,
        Existing::final_stage | Incoming::final_stage,
        Existing::final_access | Incoming::final_access,
        Existing::channel_mask | Incoming::channel_mask>;
};

template <typename List, typename T>
struct MergeUsageIntoList;

template <typename... Ts, typename T>
struct MergeUsageIntoList<TypeList<Ts...>, T> {
    static consteval auto Select() {
        if constexpr ((std::is_same_v<typename Ts::Resource, typename T::Resource> || ...)) {
            return std::type_identity<TypeList<typename CombineSameResourceUsage<Ts, T>::type...>> {};
        } else {
            return std::type_identity<TypeList<Ts..., T>> {};
        }
    }
    using type = typename decltype(Select())::type;
};

template <typename List1>
struct MergeLists<List1, TypeList<>> {
    using type = List1;
};

template <typename List1, typename Head, typename... Tail>
struct MergeLists<List1, TypeList<Head, Tail...>> {
    using HeadResources = typename MergeUsageIntoList<List1, Head>::type;
    using type          = typename MergeLists<HeadResources, TypeList<Tail...>>::type;
};

template <typename... Usages>
struct ExtractResources<TypeList<Usages...>> {
    using type = TypeList<typename Usages::Resource...>;
};

template <typename Accumulated, typename... Usages>
struct UniqueResourcesFold {
    using type = Accumulated;
};

template <typename Accumulated, typename Head, typename... Tail>
struct UniqueResourcesFold<Accumulated, Head, Tail...> {
    using Next = typename AppendUnique<Accumulated, typename Head::Resource>::type;
    using type = typename UniqueResourcesFold<Next, Tail...>::type;
};

template <typename Accumulated, typename Usages>
struct CollectResources;

template <typename Accumulated, typename... Usages>
struct CollectResources<Accumulated, TypeList<Usages...>> {
    using type = typename UniqueResourcesFold<Accumulated, Usages...>::type;
};

template <typename Accumulated, typename... Passes>
struct CollectAllResourcesFold {
    using type = Accumulated;
};

template <typename Accumulated, typename Head, typename... Tail>
struct CollectAllResourcesFold<Accumulated, Head, Tail...> {
    using WithHead = typename CollectResources<Accumulated, typename Head::Usages>::type;
    using type     = typename CollectAllResourcesFold<WithHead, Tail...>::type;
};

template <typename... Passes>
struct CollectAllResources {
    using type = typename CollectAllResourcesFold<TypeList<>, Passes...>::type;
};

template <typename Target, typename... Ts>
consteval auto GetResourceIndexImpl(TypeList<Ts...> ) -> size_t {
    size_t                idx   = 0;
    [[maybe_unused]] bool found = ((std::is_same_v<Target, Ts> ? true : (++idx, false)) || ...);
    return idx;
}

template <typename ResourceList, typename Target>
consteval auto GetResourceIndex() -> size_t {
    if constexpr (std::is_same_v<Target, detail::DummyResource> || std::is_same_v<Target, EmptyResourceTag<>>) {
        return 0;
    } else {
        constexpr size_t idx = GetResourceIndexImpl<Target>(ResourceList {});

        if constexpr (idx >= ResourceList::size) {
            constexpr auto error_message = []() consteval {
                ConstexprString<2048> msg {};
                msg.append("\n\n");
                msg.append("================================================================================\n");
                msg.append("  [GRAPH COMPILATION ERROR]\n");
                msg.append("================================================================================\n\n");
                msg.append("  The requested resource is not registered in this Frame Graph!\n\n");
                msg.append("  Missing Resource:\n");
                msg.append("    - \"");
                msg.append(std::string_view(Target::name.value.data(), Target::name.value.size()));
                msg.append("\"\n\n");
                msg.append("  Registered Resources in this Graph:\n");

                auto append_name = [&]<typename R>() {
                    msg.append("    - \"");
                    msg.append(std::string_view(R::name.value.data(), R::name.value.size()));
                    msg.append("\"\n");
                };

                [&]<typename... Us>(TypeList<Us...>) { (append_name.template operator()<Us>(), ...); }(ResourceList {});

                msg.append("================================================================================\n\n");
                return msg;
            }();

            static_assert(idx < ResourceList::size, error_message.string_view());
        }

        return idx;
    }
}

template <typename ResourceList, typename... Passes>
consteval auto ComputeStateTable() {
    constexpr size_t num_passes    = sizeof...(Passes);
    constexpr size_t num_resources = ResourceList::size;

    std::array<std::array<ResourceState, num_resources>, num_passes> table {};
    if constexpr (num_passes == 0 || num_resources == 0) {
        return table;
    } else {
    std::array<ResourceState, num_resources>                         current_states {};

    [&]<size_t... Is>(std::index_sequence<Is...>) {
        (
            [&]<size_t I>() {
                using R = typename ResourceList::template type<I>;

                constexpr bool is_written = []() consteval {
                    bool written    = false;
                    auto check_pass = [&]<typename Pass>() {
                        using Usages = typename Pass::Usages;
                        written |= []<size_t... Js>(std::index_sequence<Js...>) {
                            if constexpr (sizeof...(Js) == 0) {
                                return false;
                            } else {
                                return (
                                    (std::is_same_v<typename Usages::template type<Js>::Resource, R> &&
                                     ((Usages::template type<Js>::access & WriteMask) != 0)) ||
                                    ...
                                );
                            }
                        }(std::make_index_sequence<Usages::size> {});
                    };
                    (check_pass.template operator()<Passes>(), ...);
                    return written;
                }();

                if constexpr (!is_written) {

                    constexpr auto first_usage_info = []() consteval {
                        VkImageLayout         layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        VkPipelineStageFlags2 stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                        VkAccessFlags2        access = VK_ACCESS_2_SHADER_READ_BIT;
                        bool                  found  = false;

                        auto check_pass = [&]<typename Pass>() {
                            if (found) {
                                return;
                            }
                            using Usages = typename Pass::Usages;
                            [&]<size_t... Js>(std::index_sequence<Js...>) {
                                if constexpr (sizeof...(Js) > 0) {
                                    (([&]() {
                                         if (found) {
                                             return;
                                         }
                                         using U = typename Usages::template type<Js>;
                                         if constexpr (std::is_same_v<typename U::Resource, R>) {
                                             layout = U::layout;
                                             stage  = U::stage;
                                             access = U::access;
                                             found  = true;
                                         }
                                     }()),
                                     ...);
                                }
                            }(std::make_index_sequence<Usages::size> {});
                        };
                        (check_pass.template operator()<Passes>(), ...);
                        return ResourceState {.layout = layout, .stage = stage, .access = access, .lastWritePass = 999999, .fromPreviousFrame = false};
                    }();

                    current_states[I] = first_usage_info;
                } else {

                    current_states[I] = ResourceState {
                        .layout            = VK_IMAGE_LAYOUT_UNDEFINED,
                        .stage             = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                        .access            = 0,
                        .lastWritePass     = 999999,
                        .fromPreviousFrame = false
                    };
                }
            }.template operator()<Is>(),
            ...);
    }(std::make_index_sequence<num_resources> {});

    auto simulate_passes = [&](bool record) noexcept(false) {
        size_t pass_idx     = 0;
        auto   process_pass = [&]<typename Pass>() noexcept(false) {
            if (record) {
                table[pass_idx] = current_states;
            }

            using Usages = typename Pass::Usages;

            if constexpr (Usages::size > 0) {
                [&]<size_t... Is>(std::index_sequence<Is...>) {
                    (
                        [&]<size_t I>() {
                            using U                = typename Usages::template type<I>;
                            using Img              = typename U::Resource;
                            constexpr size_t r_idx = GetResourceIndex<ResourceList, Img>();

                            constexpr bool is_write = (U::access & WriteMask) != 0;

                            current_states[r_idx] = ResourceState {
                                .layout            = U::final_layout,
                                .stage             = U::final_stage,
                                .access            = U::final_access,
                                .lastWritePass     = is_write ? pass_idx : current_states[r_idx].lastWritePass,
                                .fromPreviousFrame = is_write ? false : current_states[r_idx].fromPreviousFrame
                            };
                        }.template operator()<Is>(),
                        ...);
                }(std::make_index_sequence<Usages::size> {});
            }

            pass_idx++;
        };

        (process_pass.template operator()<Passes>(), ...);
    };

    simulate_passes(false);

    [&]<size_t... Is>(std::index_sequence<Is...>) {
        (
            [&]<size_t I>() {
                using R = typename ResourceList::template type<I>;

                constexpr bool is_written = []() consteval {
                    bool written    = false;
                    auto check_pass = [&]<typename Pass>() {
                        using Usages = typename Pass::Usages;
                        written |= []<size_t... Js>(std::index_sequence<Js...>) {
                            if constexpr (sizeof...(Js) == 0) {
                                return false;
                            } else {
                                return (
                                    (std::is_same_v<typename Usages::template type<Js>::Resource, R> &&
                                     ((Usages::template type<Js>::access & WriteMask) != 0)) ||
                                    ...
                                );
                            }
                        }(std::make_index_sequence<Usages::size> {});
                    };
                    (check_pass.template operator()<Passes>(), ...);
                    return written;
                }();

                if constexpr (R::is_swapchain) {
                    current_states[I] = ResourceState {
                        .layout            = VK_IMAGE_LAYOUT_UNDEFINED,
                        .stage             = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        .access            = 0,
                        .lastWritePass     = 999999,
                        .fromPreviousFrame = false
                    };
                } else if constexpr (is_written && !R::is_persistent) {
                    current_states[I] = ResourceState {
                        .layout = VK_IMAGE_LAYOUT_UNDEFINED, .stage = VK_PIPELINE_STAGE_2_NONE, .access = 0, .lastWritePass = 999999, .fromPreviousFrame = false
                    };
                } else if constexpr (!is_written) {

                } else {
                    if (current_states[I].lastWritePass < 999999) {
                        current_states[I].fromPreviousFrame = true;
                    }
                }
            }.template operator()<Is>(),
            ...);
    }(std::make_index_sequence<num_resources> {});

    simulate_passes(true);

    return table;
    }
}

template <typename T>
[[nodiscard]] constexpr VkExtent3D ToExtent3D(const T& extent) noexcept {
    if constexpr (requires { extent.depth; }) {
        return extent;
    } else {
        return {extent.width, extent.height, 1};
    }
}

template <size_t Offset, size_t Len, typename... Passes>
constexpr auto ExtractForkRun(std::tuple<Passes...>& passes) noexcept {
    if constexpr (Len == 1) {
        return std::move(std::get<Offset>(passes));
    } else {
        return [&]<size_t... Ks>(std::index_sequence<Ks...>) {
            return ParallelPass<Passes...[Offset + Ks]...>(std::move(std::get<Offset + Ks>(passes))...);
        }(std::make_index_sequence<Len> {});
    }
}

}

template <typename ResourceList>
template <typename Image>
constexpr void ResourceBinder<ResourceList>::Bind(ImageSlice slice) noexcept {
    constexpr size_t idx = TemplatedDetail::GetResourceIndex<ResourceList, Image>();
    _resources[idx]      = slice;
}

template <typename ResourceList>
template <typename ContextImpl>
constexpr void ResourceBinder<ResourceList>::AutoBind(ContextImpl& impl) noexcept {
    // Explicit resolvers cover presentation depth, swapchain, history and
    // shadow views (which need more than the graph target's default view).
    [&]<typename... Resources>(TypeList<Resources...>) noexcept {
        ([&] noexcept {
            if constexpr (requires { ResourceResolver<Resources>::Resolve(impl); })
                this->template Bind<Resources>(ResourceResolver<Resources>::Resolve(impl));
        }(), ...);
    }(ResourceList {});

    // C++26 field reflection visits the members at compile time. Type identity
    // selects only requested targets; there is no per-frame string lookup.
    Reflect::ForEachField(impl.graphResources, [&]<typename Target>(const Target& rt) noexcept {
        using Resource = typename Target::TagType;
        if constexpr (IsInList<ResourceList, Resource>::value &&
                      !requires { ResourceResolver<Resource>::Resolve(impl); }) {
            this->template Bind<Resource>(rt.AsSlice());
        }
    });
}

template <typename ResourceList>
constexpr auto ResourceBinder<ResourceList>::GetBindings() const noexcept -> std::span<const ImageSlice, ResourceList::size> {
    return _resources;
}

template <typename... Passes>
constexpr auto PassPack<Passes...>::BuildGraph() && {
    if constexpr (sizeof...(Passes) == 0) {
        return CompileTimeFrameGraph<Passes...> {};
    } else {
        constexpr auto plan = TemplatedDetail::ComputeForkPlan<ResourcesOf<Passes...>, Passes...>();
        return [&]<size_t... Rs>(std::index_sequence<Rs...>) {
            return CompileTimeFrameGraph(TemplatedDetail::ExtractForkRun<plan.offsets[Rs], plan.lengths[Rs]>(passes)...);
        }(std::make_index_sequence<plan.numRuns> {});
    }
}

template <typename... Passes>
constexpr CompileTimeFrameGraph<Passes...>::CompileTimeFrameGraph(Passes&&... passes): _passes(std::move(passes)...) {
}

template <typename... Passes>
template <typename ProfilerT, typename DiagnosticsT, typename ForkPolicyT>
void CompileTimeFrameGraph<Passes...>::Execute(
    VkCommandBuffer cmd,
    const Binder&   binder,
    uint32_t        frameIndex,
    ProfilerT*      profiler,
    DiagnosticsT*   diagnostics,
    ForkPolicyT*    forker
) const {
    const auto bindings = binder.GetBindings();

    std::apply(
        [&](const auto&... passPack) noexcept(false) {
            [&]<size_t... Is>(std::index_sequence<Is...>) noexcept(false) {
                (ExecutePass<Is>(cmd, bindings, passPack...[Is], frameIndex, profiler, diagnostics, forker), ...);
            }(std::make_index_sequence<NumPasses> {});
        },
        _passes
    );
}

template <typename... Passes>
template <typename ProfilerT>
void CompileTimeFrameGraph<Passes...>::WriteScopeStart(VkCommandBuffer cmd, uint32_t frameIndex, std::string_view passName, ProfilerT* profiler) noexcept {
    if constexpr (!std::is_void_v<ProfilerT>) {
        static_assert(requires { typename ProfilerT::StageType; }, "Frame graph profilers must expose their reflected enum as StageType.");
        using ProfileStage           = typename ProfilerT::StageType;
        constexpr auto with_value    = [](std::string_view sv) constexpr { return Reflect::StringToEnum<ProfileStage>(sv); };
        const auto     profile_stage = with_value(passName);
        if (profile_stage.has_value() && profiler != nullptr) {
            static_assert(
                requires(ProfilerT& backend, ProfileStage stage) { backend.WriteStart(cmd, frameIndex, stage); },
                "Frame graph profilers must provide WriteStart(VkCommandBuffer, uint32_t, StageType)."
            );
            profiler->WriteStart(cmd, frameIndex, *profile_stage);
        }
    }
}

template <typename... Passes>
template <typename ProfilerT>
void CompileTimeFrameGraph<Passes...>::WriteScopeEnd(VkCommandBuffer cmd, uint32_t frameIndex, std::string_view passName, ProfilerT* profiler) noexcept {
    if constexpr (!std::is_void_v<ProfilerT>) {
        using ProfileStage       = typename ProfilerT::StageType;
        const auto profile_stage = Reflect::StringToEnum<ProfileStage>(passName);
        if (profile_stage.has_value() && profiler != nullptr) {
            static_assert(
                requires(ProfilerT& backend, ProfileStage stage) { backend.WriteEnd(cmd, frameIndex, stage); },
                "Frame graph profilers must provide WriteEnd(VkCommandBuffer, uint32_t, StageType)."
            );
            profiler->WriteEnd(cmd, frameIndex, *profile_stage);
        }
    }
}

template <typename... Passes>
template <size_t PassIndex, typename PassType, typename ProfilerT, typename DiagnosticsT, typename ForkPolicyT>
void CompileTimeFrameGraph<Passes...>::ExecutePass(
    VkCommandBuffer                                cmd,
    std::span<const ImageSlice, NumResources> bindings,
    const PassType&                                pass,
    uint32_t                                       frameIndex,
    ProfilerT*                                     profiler,
    DiagnosticsT*                                  diagnostics,
    ForkPolicyT*                                   forker
) const {
    constexpr std::string_view pass_name = PassType::name.string_view();

    if constexpr (!std::is_void_v<DiagnosticsT>) {
        static_assert(
            requires(DiagnosticsT& backend) { backend.WriteCheckpoint(cmd, pass_name); },
            "Frame graph diagnostics must provide WriteCheckpoint(VkCommandBuffer, std::string_view)."
        );
        if (diagnostics != nullptr) {
            diagnostics->WriteCheckpoint(cmd, pass_name);
        }
    }

    if constexpr (requires { PassType::is_fork; }) {

        std::apply(
            [&](const auto&... sub) { (WriteScopeStart(cmd, frameIndex, std::decay_t<decltype(sub)>::name.string_view(), profiler), ...); }, pass.subPasses
        );
    } else {
        WriteScopeStart(cmd, frameIndex, pass_name, profiler);
    }

    using Usages = typename PassType::Usages;

    constexpr auto barrier_plan = BuildBarrierPlan<PassIndex, PassType>();

    if constexpr (barrier_plan.count > 0) {
        std::array<VkImageMemoryBarrier2, barrier_plan.count> barriers {};

        [&]<size_t... Bs>(std::index_sequence<Bs...>) {
            (
                [&]() {
                    constexpr size_t us = barrier_plan.indices[Bs];
                    using UsageType     = typename Usages::template type<us>;
                    using Img           = typename UsageType::Resource;

                    constexpr size_t                         r_idx      = TemplatedDetail::GetResourceIndex<Resources, Img>();
                    constexpr TemplatedDetail::ResourceState prev_state = StateTable[PassIndex][r_idx];
                    const auto&                              resource   = bindings[r_idx];

                    barriers[Bs] = MakeImageBarrier({
                        .image      = resource.image,
                        .src_access = prev_state.access,
                        .dst_access = UsageType::access,
                        .src_layout = prev_state.layout,
                        .dst_layout = UsageType::layout,
                        .src_stage  = prev_state.stage,
                        .dst_stage  = UsageType::stage,
                        .aspect     = Img::aspect,
                        .base_mip   = 0,
                        .mip_count  = VK_REMAINING_MIP_LEVELS,
                    });
                }(),
                ...);
        }(std::make_index_sequence<barrier_plan.count> {});

        PipelineBarrier(cmd, {}, barriers);
    }

    if constexpr (requires { PassType::is_fork; }) {

        static_assert(ForkRecorder<ForkPolicyT>, "A fork executor must provide ExecuteFork(VkCommandBuffer, std::span<const ForkCall>) noexcept.");

        const auto                     bodyStorage = pass.Bodies();
        const std::span<const ForkCall> bodies {bodyStorage};

        if (forker != nullptr && bodies.size() > 1) {
            forker->ExecuteFork(cmd, bodies);
        } else {
            for (const ForkCall& body: bodies) {
                body(cmd);
            }
        }

        std::apply(
            [&](const auto&... sub) { (WriteScopeEnd(cmd, frameIndex, std::decay_t<decltype(sub)>::name.string_view(), profiler), ...); }, pass.subPasses
        );
    } else {

        static_assert(
            FrameGraphPass<PassType>,
            "a graph pass must either be a Vk::Fork group (static constexpr is_fork) or satisfy Vk::FrameGraphPass by inheriting Vk::RenderPass<\"Name\", Usages...>"
        );

        using ColorWrites = TemplatedDetail::Filter<Usages, TemplatedDetail::IsColorAttachment>;
        using DepthWrites = TemplatedDetail::Filter<Usages, TemplatedDetail::IsDepthAttachment>;

        constexpr bool is_raster = (ColorWrites::size > 0) || (DepthWrites::size > 0);

        if constexpr (is_raster) {
            using AutoCtx = RasterPassContext<Resources, ColorWrites, DepthWrites, PassIndex, Passes...>;

            if constexpr (std::is_invocable_v<PassType, AutoCtx&>) {
                AutoCtx ctx(cmd, bindings);
                pass(ctx);
            }

            else if constexpr (std::is_invocable_v<PassType, VkCommandBuffer>) {
                pass(cmd);
            } else {
                static_assert(
                    TemplatedDetail::DependentFalse<PassType>,
                    "raster pass must implement operator()(RasterPassContext&) or operator()(VkCommandBuffer)"
                );
            }
        } else {

            static_assert(std::is_invocable_v<PassType, VkCommandBuffer>, "compute/transfer pass must implement operator()(VkCommandBuffer)");

            if constexpr (std::is_invocable_v<PassType, VkCommandBuffer>) {
                pass(cmd);
            }
        }

        WriteScopeEnd(cmd, frameIndex, pass_name, profiler);
    }
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::RasterPassContext(
    VkCommandBuffer                                      cmd,
    std::span<const ImageSlice, ResourceList::size> bindings
) noexcept: RasterPassContextBase(cmd) {
    SetExtent({});
    ResolveExtent(bindings, ColorWrites {}, DepthWrites {});

    uint32_t color_count = 0;
    BuildColorAttachments(bindings, color_count, ColorWrites {});

    VkRenderingAttachmentInfo depth_attachment {};
    bool                      has_depth = BuildDepthAttachment(bindings, depth_attachment, DepthWrites {});

    VkRenderingInfo rendering_info = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext                = nullptr,
        .flags                = 0,
        .renderArea           = {.offset = {.x = 0, .y = 0}, .extent = Extent()},
        .layerCount           = 1,
        .viewMask             = 0,
        .colorAttachmentCount = color_count,
        .pColorAttachments    = color_count > 0 ? m_colors.data() : nullptr,
        .pDepthAttachment     = has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment   = nullptr,
    };

    vkCmdBeginRendering(Cmd(), &rendering_info);

    const VkViewport viewport = {
        .x = 0.0F, .y = 0.0F, .width = static_cast<float>(Extent().width), .height = static_cast<float>(Extent().height), .minDepth = 0.0F, .maxDepth = 1.0F
    };
    const VkRect2D scissor = {.offset = {.x = 0, .y = 0}, .extent = Extent()};
    vkCmdSetViewport(Cmd(), 0, 1, &viewport);
    vkCmdSetScissor(Cmd(), 0, 1, &scissor);
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::~RasterPassContext() noexcept {
    vkCmdEndRendering(Cmd());
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
template <typename... Imgs, typename... DImgs>
void RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::ResolveExtent(
    std::span<const ImageSlice, ResourceList::size> bindings,
    TypeList<Imgs...> ,
    TypeList<DImgs...>
) noexcept {
    (([&]() {
         if (Extent().width == 0) {
             using Img       = Imgs;
             const auto& ext = bindings[TemplatedDetail::GetResourceIndex<ResourceList, Img>()].extent;

             SetExtent({ext.width, ext.height});
         }
     }()),
     ...);

    if (Extent().width == 0) {
        (([&]() {
             if (Extent().width == 0) {
                 using Img       = DImgs;
                 const auto& ext = bindings[TemplatedDetail::GetResourceIndex<ResourceList, Img>()].extent;

                 SetExtent({ext.width, ext.height});
             }
         }()),
         ...);
    }
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
template <typename... Imgs>
void RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::
    BuildColorAttachments(std::span<const ImageSlice, ResourceList::size> bindings, uint32_t& colorCount, TypeList<Imgs...> ) noexcept {
    (([&]() {
         using Img              = Imgs;
         constexpr size_t r_idx = TemplatedDetail::GetResourceIndex<ResourceList, Img>();
         const auto&      res   = bindings[r_idx];

         constexpr auto     prev_state = CompileTimeFrameGraph<Passes...>::StateTable[PassIndex][r_idx];
         VkAttachmentLoadOp load_op    = (prev_state.layout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;

         constexpr Color4 clear_color = ClearColorOf<Img>::value;

         m_colors[colorCount++] = {
             .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
             .pNext              = nullptr,
             .imageView          = res.attachmentView,
             .imageLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
             .resolveMode        = VK_RESOLVE_MODE_NONE,
             .resolveImageView   = VK_NULL_HANDLE,
             .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
             .loadOp             = load_op,
             .storeOp            = VK_ATTACHMENT_STORE_OP_STORE,
             .clearValue         = {.color = {.float32 = {clear_color.r, clear_color.g, clear_color.b, clear_color.a}}}
         };
     }()),
     ...);
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
template <typename... DImgs>
bool RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::BuildDepthAttachment(
    std::span<const ImageSlice, ResourceList::size> bindings,
    VkRenderingAttachmentInfo&                           outDepth,
    TypeList<DImgs...>
) noexcept {
    if constexpr (DepthWrites::size == 0) {
        return false;
    } else {

        using Img              = DImgs...[0];
        constexpr size_t r_idx = TemplatedDetail::GetResourceIndex<ResourceList, Img>();
        const auto&      res   = bindings[r_idx];

        constexpr auto     prev_state = CompileTimeFrameGraph<Passes...>::StateTable[PassIndex][r_idx];
        VkAttachmentLoadOp load_op    = (prev_state.layout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;

        outDepth = {
            .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext              = nullptr,
            .imageView          = res.attachmentView,
            .imageLayout        = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .resolveMode        = VK_RESOLVE_MODE_NONE,
            .resolveImageView   = VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp             = load_op,
            .storeOp            = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue         = {.depthStencil = {.depth = 1.0F, .stencil = 0}}
        };
        return true;
    }
}

template <typename Tag, typename T>
constexpr auto MakeRef(const T& resource) noexcept -> ImageSlice {
    if constexpr (requires { resource.AsSlice(); }) {
        return resource.AsSlice();
    } else if constexpr (requires { resource.Raw(); }) {
        return resource.Raw();
    } else if constexpr (std::is_same_v<std::remove_cvref_t<T>, ImageSlice>) {
        return resource;
    } else {
        static_assert(sizeof(T) == 0, "Unsupported resource type while making a graph reference");
    }
}

template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view) noexcept {
    return ImageSlice {handle, view, VkExtent3D {0, 0, 1}, Tag::format, Tag::aspect};
}

template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view, VkExtent2D extent, VkFormat format) noexcept {
    return ImageSlice {handle, view, extent, format, Tag::aspect};
}

template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view, VkExtent3D extent, VkFormat format) noexcept {
    return ImageSlice {handle, view, extent, format, Tag::aspect};
}

template <typename Resources, typename... Passes>
constexpr auto AutoForkPasses(std::tuple<Passes...> passes) noexcept {
    if constexpr (sizeof...(Passes) == 0) {
        return std::tuple<Passes...> {};
    } else {
        constexpr auto plan = TemplatedDetail::ComputeForkPlan<Resources, Passes...>();
        return [&]<size_t... Rs>(std::index_sequence<Rs...>) {
            return std::tuple {TemplatedDetail::ExtractForkRun<plan.offsets[Rs], plan.lengths[Rs]>(passes)...};
        }(std::make_index_sequence<plan.numRuns> {});
    }
}

}

namespace ZHLN::Vk::Debug {

template <typename UsagesList, typename Target>
struct IsResourceInUsages {
    static constexpr bool value = false;
};

template <typename... Us, typename Target>
struct IsResourceInUsages<TypeList<Us...>, Target> {
    static constexpr bool value = (std::is_same_v<typename Us::Resource, Target> || ...);
};

template <typename GraphT, size_t PassIdx, size_t ResIdx, typename Pass, typename Res, typename VisualizerStringT>
constexpr void PrintResourceState(VisualizerStringT& msg) noexcept {
    using UsageType          = typename TemplatedDetail::FindResourceUsage<typename Pass::Usages, Res>::type;
    constexpr bool is_active = !std::is_same_v<UsageType, void>;

    if constexpr (is_active) {
        constexpr auto prev_state = GraphT::StateTable[PassIdx][ResIdx];

        msg.append("      ↳ Resource [");
        msg.append_int(ResIdx);
        msg.append("] (\"");
        msg.append(std::string_view(Res::name.value.data()));
        msg.append("\")\n");

        constexpr bool layout_changed = (prev_state.layout != UsageType::layout);

        constexpr bool is_prev_write = (prev_state.access & ZHLN::Vk::TemplatedDetail::WriteMask) != 0;
        constexpr bool is_curr_write = (UsageType::access & ZHLN::Vk::TemplatedDetail::WriteMask) != 0;

        constexpr bool write_hazard = (is_curr_write && (prev_state.fromPreviousFrame || (prev_state.lastWritePass < PassIdx))) ||
                                      (is_prev_write && !is_curr_write && (prev_state.lastWritePass < PassIdx));

        constexpr bool needs_barrier = layout_changed || write_hazard || (prev_state.layout == VK_IMAGE_LAYOUT_UNDEFINED);

        if constexpr (needs_barrier) {
            msg.append("          Layout: ");
            msg.append(LayoutTraits<prev_state.layout>::kName);
            msg.append(" ➔ ");
            msg.append(LayoutTraits<UsageType::layout>::kName);
            msg.append("\n");

            msg.append("          Stage : ");
            msg.append_enum(static_cast<BarrierStage>(prev_state.stage));
            msg.append(" ➔ ");
            msg.append_enum(static_cast<BarrierStage>(UsageType::stage));
            msg.append("\n");

            msg.append("          Access: ");
            msg.append_enum(static_cast<BarrierAccess>(prev_state.access));
            msg.append(" ➔ ");
            msg.append_enum(static_cast<BarrierAccess>(UsageType::access));
            msg.append("\n");
        } else {
            msg.append("          State : ");
            msg.append(LayoutTraits<prev_state.layout>::kName);
            msg.append(" (");
            msg.append_enum(static_cast<BarrierStage>(prev_state.stage));
            msg.append(" / ");
            msg.append_enum(static_cast<BarrierAccess>(prev_state.access));
            msg.append(") [Unchanged]\n");
        }
    }
}

template <typename GraphT, size_t PassIdx, typename Pass, typename Resources, size_t NumResources, typename VisualizerStringT, size_t... ResIdxs>
constexpr void PrintResources(VisualizerStringT& msg, std::index_sequence<ResIdxs...> ) noexcept {
    [&]<typename... R>(TypeList<R...>) {
        (
            [&]<size_t ResIdx, typename Res>() {
                if constexpr (IsResourceInUsages<typename Pass::Usages, Res>::value) {
                    PrintResourceState<GraphT, PassIdx, ResIdx, Pass, Res>(msg);
                }
            }.template operator()<ResIdxs, R>(),
            ...
        );
    }(Resources {});
}

template <typename GraphT, size_t PassIdx, typename Pass, typename Resources, size_t NumResources, typename VisualizerStringT>
constexpr void PrintPassState(VisualizerStringT& msg, std::index_sequence<0> ) noexcept {
    msg.append("  ● Pass [");
    msg.append_int(PassIdx);
    msg.append("]: \"");
    msg.append(std::string_view(Pass::name.value.data()));
    msg.append("\"\n");

    PrintResources<GraphT, PassIdx, Pass, Resources, NumResources>(msg, std::make_index_sequence<NumResources> {});
}

template <typename GraphT, typename PassesTuple, typename Resources, size_t NumResources, typename VisualizerStringT, size_t... PassIdxs>
constexpr void PrintPasses(VisualizerStringT& msg, std::index_sequence<PassIdxs...> ) noexcept {
    [&]<typename... P>(std::type_identity<std::tuple<P...>>) {
        ((PrintPassState<GraphT, PassIdxs, P...[PassIdxs], Resources, NumResources>(msg, std::make_index_sequence<1> {}), msg.append("\n")), ...);
    }(std::type_identity<PassesTuple> {});
}

template <typename Resources, size_t NumResources, typename VisualizerStringT, size_t... Is>
constexpr void PrintResourceNames(VisualizerStringT& msg, std::index_sequence<Is...> ) noexcept {
    [&]<typename... R>(TypeList<R...>) {
        ((msg.append("    [Resource "), msg.append_int(Is), msg.append("]: \""), msg.append(std::string_view(R...[Is] ::name.value.data())), msg.append("\""),
          msg.append(R...[Is] ::is_swapchain ? " (SWAPCHAIN)" : ""), msg.append("\n")),
         ...);
    }(Resources {});
}

}

template <typename... Passes>
consteval auto ZHLN::Vk::Debug::GraphVisualizer<ZHLN::Vk::CompileTimeFrameGraph<Passes...>>::Visualize() {
    VisualizerString<32768> msg {};
    msg.append("\n\n");
    msg.append("================================================================================\n");
    msg.append("  [RENDER GRAPH COMPILE-TIME STATE TABLE VISUALIZATION]\n");
    msg.append("================================================================================\n\n");

    msg.append("  Total Registered Resources: ");
    msg.append_int(NumResources);
    msg.append("\n");
    msg.append("  Total Passes: ");
    msg.append_int(NumPasses);
    msg.append("\n\n");

    msg.append("  --- REGISTERED RESOURCES ---\n");
    PrintResourceNames<Resources, NumResources>(msg, std::make_index_sequence<NumResources> {});

    msg.append("\n  --- PASS STATE TRANSITIONS (BEFORE ➔ AFTER BARRIER) ---\n\n");
    PrintPasses<GraphT, std::tuple<Passes...>, Resources, NumResources>(msg, std::make_index_sequence<NumPasses> {});

    msg.append("================================================================================\n\n");
    return msg;
}
