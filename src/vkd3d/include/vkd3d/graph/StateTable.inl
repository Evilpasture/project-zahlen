// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ZHLN::VkD3D::TemplatedDetail {

template <typename Target, typename... Ts>
consteval auto GetResourceIndexImpl(TypeList<Ts...> ) -> size_t {
    size_t                idx   = 0;
    [[maybe_unused]] bool found = ((std::is_same_v<Target, Ts> ? true : (++idx, false)) || ...);
    return idx;
}

template <typename ResourceList, typename Target>
consteval auto GetResourceIndex() -> size_t {
    if constexpr (std::is_same_v<Target, DummyResource> || std::is_same_v<Target, EmptyResourceTag<>>) {
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
                                     (Has(Usages::template type<Js>::access, WriteMask))) ||
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
                        ImageLayout   layout = ImageLayout::ShaderReadOnlyOptimal;
                        PipelineStage stage  = PipelineStage::Fragment;
                        Access        access = Access::ShaderRead;
                        bool          found  = false;

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
                        .layout            = ImageLayout::Undefined,
                        .stage             = PipelineStage::TopOfPipe,
                        .access            = Access::None,
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

                            constexpr bool is_write = Has(U::access, WriteMask);

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
                                     (Has(Usages::template type<Js>::access, WriteMask))) ||
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
                        .layout            = ImageLayout::Undefined,
                        .stage             = PipelineStage::ColorAttachment,
                        .access            = Access::None,
                        .lastWritePass     = 999999,
                        .fromPreviousFrame = false
                    };
                } else if constexpr (is_written && !R::is_persistent) {
                    current_states[I] = ResourceState {
                        .layout = ImageLayout::Undefined, .stage = PipelineStage::None, .access = Access::None, .lastWritePass = 999999, .fromPreviousFrame = false
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

} // namespace ZHLN::VkD3D::TemplatedDetail
