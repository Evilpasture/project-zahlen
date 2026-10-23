// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../FrameContext.hpp"
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN::Passes {

template <typename Push, typename... Slots>
struct FullscreenPassArgs {
    Push                 push;
    std::tuple<Slots...> slots;
};

template <typename Push, typename... Slots>
[[nodiscard]] constexpr auto MakeFullscreenPassArgs(Push&& push, Slots&&... slots) noexcept -> FullscreenPassArgs<std::decay_t<Push>, std::decay_t<Slots>...> {
    return {
        .push  = std::forward<Push>(push),
        .slots = std::tuple<std::decay_t<Slots>...> {std::forward<Slots>(slots)...},
    };
}

template <typename Declared, Vk::ShaderProgram... Modules>
struct FullscreenPassRecorder {
    // Two contracts, matching the graph's two raster-pass shapes
    // (RenderGraph.inl): a RasterPassContextBase& operator() is MANAGED -- the
    // graph's RasterPassContext has already begun the rendering scope and ends
    // it after the call, so the body must never open its own; a VkCommandBuffer
    // operator() is MANUAL -- the pass wraps its own Vk::DynamicPass scope
    // (swapchain targets, DONT_CARE loads). The ctx-forms below serve managed
    // passes, the cmd-forms manual ones. Mixing them nests vkCmdBeginRendering.

    template <typename LayoutT, typename Prepare>
    static void Record(RenderFrameContext& impl, Vk::RasterPassContextBase& ctx, Vk::FullscreenPass<LayoutT>& pass, Prepare&& prepare) noexcept {
        RecordCustom(impl, ctx, pass, std::forward<Prepare>(prepare), [](auto&, VkCommandBuffer, auto&& execute) noexcept { execute(); });
    }

    template <typename LayoutT, typename Prepare>
    static void Record(RenderFrameContext& impl, VkCommandBuffer cmd, Vk::FullscreenPass<LayoutT>& pass, Prepare&& prepare) noexcept {
        RecordCustom(impl, cmd, pass, std::forward<Prepare>(prepare), [](auto&, VkCommandBuffer, auto&& execute) noexcept { execute(); });
    }

    template <typename LayoutT, typename Prepare, typename Draw>
    static void RecordCustom(RenderFrameContext& impl, VkCommandBuffer cmd, Vk::FullscreenPass<LayoutT>& pass, Prepare&& prepare, Draw&& draw) noexcept {
        // Manual shape: the raw handle enters here and nowhere further. The
        // boundary check is the one `if` this contract allows; everything
        // below runs on the context proof.
        if (cmd == VK_NULL_HANDLE) [[unlikely]] {
            return;
        }
        Vk::RasterPassContextBase ctx(cmd);
        RecordCustom(impl, ctx, pass, std::forward<Prepare>(prepare), std::forward<Draw>(draw));
    }

    template <typename LayoutT, typename Prepare, typename Draw>
    static void RecordCustom(RenderFrameContext& impl, Vk::RasterPassContextBase& ctx, Vk::FullscreenPass<LayoutT>& pass, Prepare&& prepare, Draw&& draw) noexcept {
        // Record-time pruning: an unbuilt pass produces no ActivePipeline and
        // never reaches the draw; the draw itself holds the proof, not a flag.
        const auto active = Vk::ActivePipeline::From(pass.pipeline);
        if (!active) {
            return;
        }

        const auto args  = std::invoke(std::forward<Prepare>(prepare));
        const auto block = std::apply(
            [&impl, &pass](const auto&... slots) noexcept { return pass.template WriteHeapParameters<Declared>(impl.heapManager, slots...); }, args.slots
        );

        // The heap-index proof: Create refuses the zero offset a mis-reflected
        // pass would otherwise push with.
        const auto heap = Vk::BoundHeapPass::Create(pass.heapBindings.indexPushOffset, block.slot);
        if (!heap) {
            return;
        }

        const auto execute = [&]() noexcept { pass.template ExecuteHeap<Modules...>(ctx, *active, *heap, args.push); };
        std::invoke(std::forward<Draw>(draw), impl, ctx.Cmd(), execute);
    }
};

} // namespace ZHLN::Passes
