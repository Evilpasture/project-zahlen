// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "graph/RenderGraph.hpp"
#include "passes/GraphPassUtils.hpp"
#include <ShaderBindings.hpp>
#include <concepts>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN::Passes {

// What Desc::Prepare returns: the push payload (absent when the shader
// declares no push block), the dispatch extent, and the named slot writes.
template <typename Push, typename... Slots>
struct ComputePassArgs {
    Push                 push;
    VkExtent3D           extent;
    std::tuple<Slots...> slots;
};

template <typename... Slots>
struct ComputePassArgs<void, Slots...> {
    VkExtent3D           extent;
    std::tuple<Slots...> slots;
};

template <typename Push, typename... Slots>
[[nodiscard]] constexpr auto MakeComputePassArgs(const Push& push, VkExtent3D extent, Slots&&... slots) noexcept
    -> ComputePassArgs<Push, std::decay_t<Slots>...> {
    return {
        .push  = push,
        .extent = extent,
        .slots = std::tuple<std::decay_t<Slots>...> {std::forward<Slots>(slots)...},
    };
}

template <typename... Slots>
[[nodiscard]] constexpr auto MakeComputePassArgs(VkExtent3D extent, Slots&&... slots) noexcept -> ComputePassArgs<void, std::decay_t<Slots>...> {
    return {
        .extent = extent,
        .slots = std::tuple<std::decay_t<Slots>...> {std::forward<Slots>(slots)...},
    };
}

namespace TemplatedDetail {

// The two compute-pass shapes the engine dispatches. PostProcessFeature owns
// heap-indexed dynamic passes dispatched by thread count; VolumetricFogSystem
// owns double-buffered fixed-domain passes whose dispatch size is reflected
// from the shader. A descriptor names its pass and the template records the
// matching dispatch.
template <typename T>
struct IsDoubleBufferedPass: std::false_type {};
template <typename LayoutT, Vk::ComputeDomain Domain>
struct IsDoubleBufferedPass<Vk::DoubleBufferedComputePass<LayoutT, Domain>>: std::true_type {};

} // namespace TemplatedDetail

// One single-dispatch compute pass, as a descriptor plus this executor. The
// descriptor (GtaoDesc, VolumetricFogInjectDesc, ...) states what differs
// between passes: the graph name and usage list, the descriptor set and
// module, the push payload, the early-out, the heap bindings, and the slot
// writes; this template records the shared skeleton -- gate, bind heaps,
// write the heap block, dispatch.
//
// The descriptor contract:
//   static constexpr auto              name;          // matches a Stage enumerator
//   using Usages                       = Vk::TypeList<...>;
//   using ShaderDecl                   = Shaders::X;  // descriptor set
//   using ShaderModule                 = Shaders::Modules::X;
//   using PushConstants                = T | void;
//   static auto GetPass(frame)         -> pass&;      // pipeline
//   static auto GetBindings(frame)     -> heap bindings&
//   static auto Prepare(frame, pc)     -> ComputePassArgs<...>;
//   static bool ShouldRun(frame);                     // optional early-out
template <typename Desc>
struct ComputePass: TemplatedDetail::RenderPassOf<Desc::name, typename Desc::Usages> {
    static_assert(TemplatedDetail::kNameProfilesAsStage<Desc::name>, "the pass name has no Stage enumerator, so the GPU profiler would never time it");

    RenderFrameContext&           frame;
    Slang::ScenePassPushConstants pc {};

    void operator()(VkCommandBuffer cmd) const noexcept {
        if constexpr (requires(RenderFrameContext& f) {
                          {
                              Desc::ShouldRun(f)
                          } -> std::convertible_to<bool>;
                      }) {
            if (!Desc::ShouldRun(frame)) {
                return;
            }
        }

        frame.BindHeapsAndPushFrame(cmd);

        const auto args  = Desc::Prepare(frame, pc);
        const auto block = std::apply(
            [&](const auto&... slots) noexcept {
                return frame.heapManager.template WriteHeapParameters<typename Desc::ShaderDecl>(Desc::GetBindings(frame), slots...);
            },
            args.slots
        );

        auto& pass = Desc::GetPass(frame);
        if constexpr (TemplatedDetail::IsDoubleBufferedPass<std::remove_cvref_t<decltype(pass)>>::value) {
            // Fixed domain: the dispatch size comes from the shader, so the
            // extent is recorded but not consumed.
            if constexpr (!std::is_void_v<typename Desc::PushConstants>) {
                pass.template DispatchHeap<typename Desc::ShaderModule>(cmd, block, args.push);
            } else {
                pass.DispatchHeap(cmd, block);
            }
        } else {
            if constexpr (!std::is_void_v<typename Desc::PushConstants>) {
                pass.template DispatchHeapIndexedThreads<typename Desc::ShaderModule>(
                    cmd, block, args.extent.width, args.extent.height, args.extent.depth, args.push
                );
            } else {
                pass.DispatchHeapIndexedThreads(cmd, block, args.extent.width, args.extent.height, args.extent.depth);
            }
        }
    }
};

} // namespace ZHLN::Passes
