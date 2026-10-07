// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../RenderInternal.hpp"
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN::Passes {

// The algorithm-specific part of a fullscreen draw: its push payload and its
// named descriptor slots. The recorder owns the common valid/bind/draw path.
template <typename Push, typename... Slots>
struct FullscreenPassArgs {
    Push                 push;
    std::tuple<Slots...> slots;
};

template <typename Push, typename... Slots>
[[nodiscard]] constexpr auto MakeFullscreenPassArgs(Push&& push, Slots&&... slots) noexcept
    -> FullscreenPassArgs<std::decay_t<Push>, std::decay_t<Slots>...> {
    return {
        .push  = std::forward<Push>(push),
        .slots = std::tuple<std::decay_t<Slots>...> {std::forward<Slots>(slots)...},
    };
}

template <typename Declared, Vk::ShaderProgram... Modules>
struct FullscreenPassRecorder {
    template <typename LayoutT, typename Prepare>
    static void Record(
        RenderContext::Impl&                  impl,
        VkCommandBuffer                        cmd,
        Vk::FullscreenPass<LayoutT>&           pass,
        Prepare&&                              prepare
    ) noexcept {
        RecordCustom(
            impl, cmd, pass, std::forward<Prepare>(prepare),
            [](auto&, VkCommandBuffer, auto&& execute) noexcept { execute(); }
        );
    }

    // Use when the fullscreen draw needs an outer recording scope, e.g. the
    // final blit into the current swapchain image's dynamic-rendering scope.
    template <typename LayoutT, typename Prepare, typename Draw>
    static void RecordCustom(
        RenderContext::Impl&                  impl,
        VkCommandBuffer                        cmd,
        Vk::FullscreenPass<LayoutT>&           pass,
        Prepare&&                              prepare,
        Draw&&                                 draw
    ) noexcept {
        if (!pass.pipeline.Valid()) {
            return;
        }

        auto args = std::invoke(std::forward<Prepare>(prepare));
        const Vk::HeapBlockBase block = std::apply(
            [&impl, &pass](const auto&... slots) noexcept {
                return pass.template WriteHeapParameters<Declared>(impl.ctx, impl.heapManager, slots...);
            },
            args.slots
        );

        const auto execute = [&]() noexcept { pass.template ExecuteHeap<Modules...>(impl.ctx, cmd, args.push, block); };
        std::invoke(std::forward<Draw>(draw), impl, cmd, execute);
    }
};

}
