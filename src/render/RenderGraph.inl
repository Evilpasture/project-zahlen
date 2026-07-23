// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderGraph.hpp"

namespace ZHLN::Vk {

// ============================================================================
// detail Metaprogramming & Simulation Definitions
// ============================================================================

namespace detail {

template <typename UsagesList, typename TargetResource>
struct FindResourceUsage;

template <typename TargetResource>
struct FindResourceUsage<TypeList<>, TargetResource> {
    using type = void;
};

template <typename Head, typename... Tail, typename TargetResource>
struct FindResourceUsage<TypeList<Head, Tail...>, TargetResource> {
    using type =
        std::conditional_t<std::is_same_v<typename Head::Resource, TargetResource>, Head, typename FindResourceUsage<TypeList<Tail...>, TargetResource>::type>;
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

template <typename... Ts, typename T>
struct AppendUnique<TypeList<Ts...>, T> {
    static constexpr bool contains = (std::is_same_v<Ts, T> || ...);
    using type                     = std::conditional_t<contains, TypeList<Ts...>, TypeList<Ts..., T>>;
};

template <typename List1>
struct MergeLists<List1, TypeList<>> {
    using type = List1;
};

template <typename List1, typename Head, typename... Tail>
struct MergeLists<List1, TypeList<Head, Tail...>> {
    using HeadResources = typename AppendUnique<List1, Head>::type;
    using type          = typename MergeLists<HeadResources, TypeList<Tail...>>::type;
};

template <typename... Usages>
struct ExtractResources<TypeList<Usages...>> {
    using type = TypeList<typename Usages::Resource...>;
};

template <>
struct CollectAllResources<> {
    using type = TypeList<>;
};

template <typename Head, typename... Tail>
struct CollectAllResources<Head, Tail...> {
    using HeadResources = typename ExtractResources<typename Head::Usages>::type;
    using TailResources = typename CollectAllResources<Tail...>::type;
    using type          = typename MergeLists<HeadResources, TailResources>::type;
};

template <typename Target, typename... Ts>
consteval auto GetResourceIndexImpl(TypeList<Ts...> /*unused*/) -> size_t {
    size_t                idx   = 0;
    [[maybe_unused]] bool found = ((std::is_same_v<Target, Ts> ? true : (++idx, false)) || ...);
    return idx;
}

template <size_t Capacity>
constexpr void ConstexprString<Capacity>::append(std::string_view sv) noexcept {
    size_t to_copy = std::min(sv.size(), Capacity - 1 - length);
    for (size_t i = 0; i < to_copy; ++i) {
        data_buffer[length + i] = sv[i];
    }
    length += to_copy;
    data_buffer[length] = '\0';
}

template <size_t Capacity>
constexpr auto ConstexprString<Capacity>::string_view() const noexcept -> std::string_view {
    return std::string_view(data_buffer.data(), length);
}

template <typename ResourceList, typename Target>
consteval auto GetResourceIndex() -> size_t {
    if constexpr (std::is_same_v<Target, detail::DummyResource>) {
        return 0; // Dummy resources bypass index lookup safely
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
    std::array<ResourceState, num_resources>                         current_states {};

    // Warm up simulation state
    [&]<size_t... Is>(std::index_sequence<Is...>) {
        (
            [&]<size_t I>() {
                using R = typename ResourceList::template type<I>;

                // Determine if the resource is ever written in this graph
                constexpr bool is_written = []() consteval {
                    bool written    = false;
                    auto check_pass = [&]<typename Pass>() {
                        using Usages = typename Pass::Usages;
                        written |= []<size_t... Js>(std::index_sequence<Js...>) {
                            if constexpr (sizeof...(Js) == 0) {
                                return false; // Short-circuit passes with no image usages
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
                    // It is an external read-only input.
                    // Initialize its state to its first usage's layout and stage.
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
                    // Standard initialization for resources written inside this graph
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

            [&]<size_t... Is>(std::index_sequence<Is...>) {
                (
                    [&]<size_t I>() {
                        using U                = typename Usages::template type<I>;
                        using Img              = typename U::Resource;
                        constexpr size_t r_idx = GetResourceIndex<ResourceList, Img>();

                        constexpr bool is_write = (U::access & WriteMask) != 0;

                        current_states[r_idx] = ResourceState {
                            .layout            = U::layout,
                            .stage             = U::stage,
                            .access            = U::access,
                            .lastWritePass     = is_write ? pass_idx : current_states[r_idx].lastWritePass,
                            .fromPreviousFrame = is_write ? false : current_states[r_idx].fromPreviousFrame
                        };
                    }.template operator()<Is>(),
                    ...);
            }(std::make_index_sequence<Usages::size> {});

            pass_idx++;
        };

        (process_pass.template operator()<Passes>(), ...);
    };

    simulate_passes(false);

    [&]<size_t... Is>(std::index_sequence<Is...>) {
        (
            [&]<size_t I>() {
                using R = typename ResourceList::template type<I>;

                // Determine if the resource is ever written in this graph
                constexpr bool is_written = []() consteval {
                    bool written    = false;
                    auto check_pass = [&]<typename Pass>() {
                        using Usages = typename Pass::Usages;
                        written |= []<size_t... Js>(std::index_sequence<Js...>) {
                            if constexpr (sizeof...(Js) == 0) {
                                return false; // Short-circuit passes with no image usages
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
                    // Keep the external read-only input state intact
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

template <typename T>
[[nodiscard]] constexpr VkExtent3D ToExtent3D(const T& extent) noexcept {
    if constexpr (requires { extent.depth; }) {
        return extent; // Already 3D
    } else {
        return {extent.width, extent.height, 1}; // Promote 2D -> 3D
    }
}

} // namespace detail

// ============================================================================
// Pass Builder Definitions
// ============================================================================

template <ResourceName Name, typename... Usages, typename RecordFn>
constexpr auto MakePass(RecordFn&& record) {
    constexpr bool has_graphics = (detail::IsColorAttachment<Usages>::value || ...) || (detail::IsDepthAttachment<Usages>::value || ...);

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

    return GraphPass<Name, TypeList<Usages...>, RecordFn> {std::forward<RecordFn>(record)};
}

template <ResourceName Name, typename... Usages, typename RecordFn>
constexpr auto Passieren(RecordFn&& record, detail::BypassGraphicsCheckToken /*unused*/) {
    return GraphPass<Name, TypeList<Usages...>, RecordFn> {std::forward<RecordFn>(record)};
}

// ============================================================================
// ResourceBinder Definition
// ============================================================================

template <typename ResourceList>
template <typename Image>
constexpr void ResourceBinder<ResourceList>::Bind(VkImage handle, VkImageView view, VkExtent3D extent) noexcept {
    constexpr size_t idx = detail::GetResourceIndex<ResourceList, Image>();
    _resources[idx]      = {handle, view, extent};
}

template <typename ResourceList>
constexpr auto ResourceBinder<ResourceList>::GetBindings() const noexcept -> const std::array<GraphResource, ResourceList::size>& {
    return _resources;
}

// ============================================================================
// CompileTimeFrameGraph Definitions
// ============================================================================

template <typename... Passes>
constexpr CompileTimeFrameGraph<Passes...>::CompileTimeFrameGraph(Passes&&... passes): _passes(std::move(passes)...) {
}

template <typename... Passes>
void CompileTimeFrameGraph<Passes...>::Execute(VkCommandBuffer cmd, const Binder& binder) const {
    const auto& bindings = binder.GetBindings();

    std::apply(
        [&](const auto&... passPack) noexcept(false) {
            [&]<size_t... Is>(std::index_sequence<Is...>) noexcept(false) {
                (ExecutePass<Is>(cmd, bindings, passPack...[Is]), ...);
            }(std::make_index_sequence<NumPasses> {});
        },
        _passes
    );
}

template <typename... Passes>
template <size_t PassIndex, typename PassType>
void CompileTimeFrameGraph<Passes...>::ExecutePass(VkCommandBuffer cmd, const std::array<GraphResource, NumResources>& bindings, const PassType& pass) const {
    using Usages   = typename PassType::Usages;
    using RecordFn = typename PassType::RecordFn;

    constexpr size_t barrier_count = CountRequiredBarriers<PassIndex, PassType>();

    if constexpr (barrier_count > 0) {
        constexpr auto                                   active_indices = GetBarrierUsageIndices<PassIndex, PassType, barrier_count>();
        std::array<VkImageMemoryBarrier2, barrier_count> barriers {};

        [&]<size_t... Bs>(std::index_sequence<Bs...>) {
            (
                [&]() {
                    constexpr size_t us = active_indices[Bs];
                    using UsageType     = typename Usages::template type<us>;
                    using Img           = typename UsageType::Resource;

                    constexpr size_t                r_idx      = detail::GetResourceIndex<Resources, Img>();
                    constexpr detail::ResourceState prev_state = StateTable[PassIndex][r_idx];
                    const auto&                     resource   = bindings[r_idx];

                    barriers[Bs] = VkImageMemoryBarrier2 {
                        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                        .pNext               = nullptr,
                        .srcStageMask        = prev_state.stage,
                        .srcAccessMask       = prev_state.access,
                        .dstStageMask        = UsageType::stage,
                        .dstAccessMask       = UsageType::access,
                        .oldLayout           = prev_state.layout,
                        .newLayout           = UsageType::layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image               = resource.handle,
                        .subresourceRange    = {
                            .aspectMask     = Img::aspect,
                            .baseMipLevel   = 0,
                            .levelCount     = VK_REMAINING_MIP_LEVELS,
                            .baseArrayLayer = 0,
                            .layerCount     = VK_REMAINING_ARRAY_LAYERS
                        }
                    };
                }(),
                ...);
        }(std::make_index_sequence<barrier_count> {});

        VkDependencyInfo dep_info = {
            .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext                    = nullptr,
            .dependencyFlags          = 0,
            .memoryBarrierCount       = 0,
            .pMemoryBarriers          = nullptr,
            .bufferMemoryBarrierCount = 0,
            .pBufferMemoryBarriers    = nullptr,
            .imageMemoryBarrierCount  = static_cast<uint32_t>(barrier_count),
            .pImageMemoryBarriers     = barriers.data()
        };
        vkCmdPipelineBarrier2(cmd, &dep_info);
    }

    using ColorWrites          = detail::Filter<Usages, detail::IsColorAttachment>;
    using DepthWrites          = detail::Filter<Usages, detail::IsDepthAttachment>;
    constexpr bool is_graphics = (ColorWrites::size > 0) || (DepthWrites::size > 0);

    if constexpr (is_graphics) {
        if constexpr (std::is_invocable_v<RecordFn, VkCommandBuffer>) {
            pass.record(cmd);
        } else {
            RasterPassContext<Resources, ColorWrites, DepthWrites, PassIndex, Passes...> ctx(cmd, bindings);
            pass.record(ctx);
        }
    } else {
        pass.record(cmd);
    }
}

// ============================================================================
// RasterPassContext Definitions
// ============================================================================

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::RasterPassContext(
    VkCommandBuffer                                      cmd,
    const std::array<GraphResource, ResourceList::size>& bindings
) noexcept: m_cmd(cmd) {
    m_extent = {};
    ResolveExtent(bindings, std::make_index_sequence<ColorWrites::size> {}, std::make_index_sequence<DepthWrites::size> {});

    uint32_t color_count = 0;
    BuildColorAttachments(bindings, color_count, std::make_index_sequence<ColorWrites::size> {});

    VkRenderingAttachmentInfo depth_attachment {};
    bool                      has_depth = BuildDepthAttachment(bindings, depth_attachment, std::make_index_sequence<DepthWrites::size> {});

    VkRenderingInfo rendering_info = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext                = nullptr,
        .flags                = 0,
        .renderArea           = {.offset = {.x = 0, .y = 0}, .extent = m_extent},
        .layerCount           = 1,
        .viewMask             = 0,
        .colorAttachmentCount = color_count,
        .pColorAttachments    = color_count > 0 ? m_colors.data() : nullptr,
        .pDepthAttachment     = has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment   = nullptr,
    };

    vkCmdBeginRendering(m_cmd, &rendering_info);

    const VkViewport viewport = {.x = 0.0F, .y = 0.0F, .width = (float) m_extent.width, .height = (float) m_extent.height, .minDepth = 0.0F, .maxDepth = 1.0F};
    const VkRect2D   scissor  = {.offset = {.x = 0, .y = 0}, .extent = m_extent};
    vkCmdSetViewport(m_cmd, 0, 1, &viewport);
    vkCmdSetScissor(m_cmd, 0, 1, &scissor);
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::~RasterPassContext() noexcept {
    vkCmdEndRendering(m_cmd);
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
VkCommandBuffer RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::Cmd() const noexcept {
    return m_cmd;
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
VkExtent2D RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::Extent() const noexcept {
    return m_extent;
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
template <size_t... Is, size_t... Js>
void RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::ResolveExtent(
    const std::array<GraphResource, ResourceList::size>& bindings,
    std::index_sequence<Is...> /*unused*/,
    std::index_sequence<Js...> /*unused*/
) noexcept {
    (([&]() {
         if (m_extent.width == 0) {
             using Img       = typename ColorWrites::template type<Is>;
             const auto& ext = bindings[detail::GetResourceIndex<ResourceList, Img>()].extent;
             // Explicitly truncate the 3D extent down to 2D for attachment rendering
             m_extent = {ext.width, ext.height};
         }
     }()),
     ...);

    if (m_extent.width == 0) {
        (([&]() {
             if (m_extent.width == 0) {
                 using Img       = typename DepthWrites::template type<Js>;
                 const auto& ext = bindings[detail::GetResourceIndex<ResourceList, Img>()].extent;
                 // Explicitly truncate the 3D extent down to 2D for attachment rendering
                 m_extent = {ext.width, ext.height};
             }
         }()),
         ...);
    }
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
template <size_t... Is>
void RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::
    BuildColorAttachments(const std::array<GraphResource, ResourceList::size>& bindings, uint32_t& colorCount, std::index_sequence<Is...> /*unused*/) noexcept {
    (([&]() {
         using Img              = typename ColorWrites::template type<Is>;
         constexpr size_t r_idx = detail::GetResourceIndex<ResourceList, Img>();
         const auto&      res   = bindings[r_idx];

         constexpr auto     prev_state = CompileTimeFrameGraph<Passes...>::StateTable[PassIndex][r_idx];
         VkAttachmentLoadOp load_op    = (prev_state.layout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;

         constexpr Color4 clear_color = ClearColorOf<Img>::value;

         m_colors[colorCount++] = {
             .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
             .pNext              = nullptr,
             .imageView          = res.view,
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
template <size_t... Js>
bool RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::BuildDepthAttachment(
    const std::array<GraphResource, ResourceList::size>& bindings,
    VkRenderingAttachmentInfo&                           outDepth,
    std::index_sequence<Js...> /*unused*/
) noexcept {
    if constexpr (DepthWrites::size == 0) {
        return false;
    } else {
        using Img              = typename DepthWrites::template type<0>;
        constexpr size_t r_idx = detail::GetResourceIndex<ResourceList, Img>();
        const auto&      res   = bindings[r_idx];

        constexpr auto     prev_state = CompileTimeFrameGraph<Passes...>::StateTable[PassIndex][r_idx];
        VkAttachmentLoadOp load_op    = (prev_state.layout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;

        outDepth = {
            .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext              = nullptr,
            .imageView          = res.view,
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

// ============================================================================
// Factory Helper Definitions
// ============================================================================

template <typename Tag, typename T>
constexpr auto MakeRef(const T& resource) noexcept {
    if constexpr (requires { resource.image.Handle(); }) {
        return GraphImageRef<Tag> {
            .handle = resource.image.Handle(),
            .view   = resource.view.Get(),
            .extent = detail::ToExtent3D(resource.extent) // Unified 3D adapter
        };
    } else if constexpr (requires { resource.handle; }) {
        return GraphImageRef<Tag> {
            .handle = resource.handle,
            .view   = resource.view,
            .extent = detail::ToExtent3D(resource.extent) // Unified 3D adapter
        };
    } else {
        static_assert(sizeof(T) == 0, "Unsupported resource type while making a graph reference");
    }
}

template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view) noexcept {
    return GraphImageRef<Tag> {.handle = handle, .view = view, .extent = {0, 0, 1}};
}

// 2D manual overload (used by the Swapchain binding)
template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view, VkExtent2D extent) noexcept {
    return GraphImageRef<Tag> {
        .handle = handle, .view = view, .extent = {extent.width, extent.height, 1} // Promote 2D -> 3D
    };
}

// 3D manual overload
template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view, VkExtent3D extent) noexcept {
    return GraphImageRef<Tag> {.handle = handle, .view = view, .extent = extent};
}

} // namespace ZHLN::Vk

// ============================================================================
// Debug Tools & Compile-Time Inspection Implementations
// ============================================================================

namespace ZHLN::Vk::Debug {

// Compile-time trait to check if a resource is referenced in a pass's usages
namespace {
template <typename UsagesList, typename Target>
struct IsResourceInUsages {
    static constexpr bool value = false;
};
} // namespace

template <typename... Us, typename Target>
struct IsResourceInUsages<TypeList<Us...>, Target> {
    static constexpr bool value = (std::is_same_v<typename Us::Resource, Target> || ...);
};

template <size_t Capacity>
constexpr void VisualizerString<Capacity>::append(std::string_view sv) noexcept {
    size_t to_copy = std::min(sv.size(), Capacity - 1 - length);
    for (size_t i = 0; i < to_copy; ++i) {
        data_buffer[length + i] = sv[i];
    }
    length += to_copy;
    data_buffer[length] = '\0';
}

template <size_t Capacity>
constexpr void VisualizerString<Capacity>::append_int(size_t val) noexcept {
    if (val == 0) {
        append("0");
        return;
    }
    std::array<char, 24> temp {};
    size_t               i = 0;
    while (val > 0 && i < 23) {
        temp[i++] = '0' + (val % 10);
        val /= 10;
    }
    for (size_t j = 0; j < i / 2; ++j) {
        std::swap(temp[j], temp[i - 1 - j]);
    }
    append(std::string_view(temp.data(), i));
}

template <size_t Capacity>
constexpr auto VisualizerString<Capacity>::string_view() const noexcept -> std::string_view {
    return std::string_view(data_buffer.data(), length);
}

template <typename... Passes>
constexpr std::string_view GraphVisualizer<CompileTimeFrameGraph<Passes...>>::LayoutToString(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            return "UNDEFINED";
        case VK_IMAGE_LAYOUT_GENERAL:
            return "GENERAL";
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return "COLOR_ATTACH_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return "DEPTH_STENCIL_ATTACH_OPTIMAL";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return "SHADER_READ_ONLY";
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return "TRANSFER_SRC";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return "TRANSFER_DST";
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
            return "DEPTH_ATTACH_OPTIMAL";
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            return "PRESENT_SRC_KHR";
        default:
            return "UNKNOWN_OR_CUSTOM_LAYOUT";
    }
}

template <typename... Passes>
constexpr std::string_view GraphVisualizer<CompileTimeFrameGraph<Passes...>>::StageToString(VkPipelineStageFlags2 stage) {
    if (stage == VK_PIPELINE_STAGE_2_NONE) {
        return "NONE";
    }
    if (stage == VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT) {
        return "TOP_OF_PIPE";
    }
    if (stage == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT) {
        return "BOTTOM_OF_PIPE";
    }

    if (stage & VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
        return "COLOR_ATTACHMENT_OUTPUT";
    }
    if (stage & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) {
        return "FRAGMENT_SHADER";
    }
    if (stage & (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)) {
        return "DEPTH_STENCIL_TESTS";
    }
    if (stage & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) {
        return "COMPUTE_SHADER";
    }
    if (stage & VK_PIPELINE_STAGE_2_TRANSFER_BIT) {
        return "TRANSFER";
    }

    return "COMBINED_OR_OTHER_STAGES";
}

template <typename... Passes>
constexpr std::string_view GraphVisualizer<CompileTimeFrameGraph<Passes...>>::AccessToString(VkAccessFlags2 access) {
    if (access == 0) {
        return "NONE";
    }
    if (access & VK_ACCESS_2_SHADER_WRITE_BIT) {
        return "SHADER_WRITE";
    }
    if (access & VK_ACCESS_2_SHADER_READ_BIT) {
        return "SHADER_READ";
    }
    if (access & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) {
        return "COLOR_WRITE";
    }
    if (access & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) {
        return "DEPTH_WRITE";
    }
    if (access & VK_ACCESS_2_TRANSFER_WRITE_BIT) {
        return "TRANSFER_WRITE";
    }
    if (access & VK_ACCESS_2_TRANSFER_READ_BIT) {
        return "TRANSFER_READ";
    }
    return "COMBINED_OR_OTHER_ACCESS";
}

// 1. Standalone compile-time helper to print a single resource state
template <typename GraphT, size_t PassIdx, size_t ResIdx, typename Pass, typename Res, typename VisualizerStringT>
constexpr void PrintResourceState(VisualizerStringT& msg) noexcept {
    using UsageType          = typename detail::FindResourceUsage<typename Pass::Usages, Res>::type;
    constexpr bool is_active = !std::is_same_v<UsageType, void>;

    if constexpr (is_active) {
        constexpr auto prev_state = GraphT::StateTable[PassIdx][ResIdx];

        msg.append("      ↳ Resource [");
        msg.append_int(ResIdx);
        msg.append("] (\"");
        msg.append(std::string_view(Res::name.value.data()));
        msg.append("\")\n");

        constexpr bool layout_changed = (prev_state.layout != UsageType::layout);

        // Decouple read vs write access types using the compiler's WriteMask
        constexpr bool is_prev_write = (prev_state.access & ZHLN::Vk::detail::WriteMask) != 0;
        constexpr bool is_curr_write = (UsageType::access & ZHLN::Vk::detail::WriteMask) != 0;

        // WAW (Write-After-Write) or RAW (Read-After-Write) hazards require active synchronization.
        // Pure RAR (Read-After-Read) stage changes do not.
        constexpr bool write_hazard = (is_curr_write && (prev_state.fromPreviousFrame || (prev_state.lastWritePass < PassIdx))) ||
                                      (is_prev_write && !is_curr_write && (prev_state.lastWritePass < PassIdx));

        constexpr bool needs_barrier = layout_changed || write_hazard || (prev_state.layout == VK_IMAGE_LAYOUT_UNDEFINED);

        using Vis = GraphVisualizer<GraphT>;

        if constexpr (needs_barrier) {
            msg.append("          Layout: ");
            msg.append(Vis::LayoutToString(prev_state.layout));
            msg.append(" ➔ ");
            msg.append(Vis::LayoutToString(UsageType::layout));
            msg.append("\n");

            msg.append("          Stage : ");
            msg.append(Vis::StageToString(prev_state.stage));
            msg.append(" ➔ ");
            msg.append(Vis::StageToString(UsageType::stage));
            msg.append("\n");

            msg.append("          Access: ");
            msg.append(Vis::AccessToString(prev_state.access));
            msg.append(" ➔ ");
            msg.append(Vis::AccessToString(UsageType::access));
            msg.append("\n");
        } else {
            msg.append("          State : ");
            msg.append(Vis::LayoutToString(prev_state.layout));
            msg.append(" (");
            msg.append(Vis::StageToString(prev_state.stage));
            msg.append(" / ");
            msg.append(Vis::AccessToString(prev_state.access));
            msg.append(") [Unchanged]\n");
        }
    }
}

// 2. Standalone helper to fold over resource index sequence
template <typename GraphT, size_t PassIdx, typename Pass, typename Resources, size_t NumResources, typename VisualizerStringT, size_t... ResIdxs>
constexpr void PrintResources(VisualizerStringT& msg, std::index_sequence<ResIdxs...> /*unused*/) noexcept {
    (PrintResourceState<GraphT, PassIdx, ResIdxs, Pass, typename Resources::template type<ResIdxs>>(msg), ...);
}

// 3. Standalone helper to print a single pass state
template <typename GraphT, size_t PassIdx, typename Pass, typename Resources, size_t NumResources, typename VisualizerStringT>
constexpr void PrintPassState(VisualizerStringT& msg) noexcept {
    msg.append("  ● Pass [");
    msg.append_int(PassIdx);
    msg.append("]: \"");
    msg.append(std::string_view(Pass::name.value.data()));
    msg.append("\"\n");

    PrintResources<GraphT, PassIdx, Pass, Resources, NumResources>(msg, std::make_index_sequence<NumResources> {});
}

// 4. Standalone helper to fold over pass index sequence
template <typename GraphT, typename PassesTuple, typename Resources, size_t NumResources, typename VisualizerStringT, size_t... PassIdxs>
constexpr void PrintPasses(VisualizerStringT& msg, std::index_sequence<PassIdxs...> /*unused*/) noexcept {
    ((PrintPassState<GraphT, PassIdxs, std::tuple_element_t<PassIdxs, PassesTuple>, Resources, NumResources>(msg), msg.append("\n")), ...);
}

// 5. Standalone helper to print registered resource list
template <typename Resources, size_t NumResources, typename VisualizerStringT, size_t... Is>
constexpr void PrintResourceNames(VisualizerStringT& msg, std::index_sequence<Is...> /*unused*/) noexcept {
    ((msg.append("    [Resource "), msg.append_int(Is), msg.append("]: \""), msg.append(std::string_view(Resources::template type<Is>::name.value.data())),
      msg.append("\""), msg.append(Resources::template type<Is>::is_swapchain ? " (SWAPCHAIN)" : ""), msg.append("\n")),
     ...);
}

} // namespace ZHLN::Vk::Debug

// ============================================================================
// Main Visualize Entry Point
// ============================================================================

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
