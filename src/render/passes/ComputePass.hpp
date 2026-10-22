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

// The two compute-pass shapes the engine dispatches. LightingResources and
// PostProcessResources own heap-indexed dynamic passes dispatched by thread
// count; VolumetricFogSystem owns double-buffered fixed-domain passes whose
// dispatch size is reflected from the shader. A descriptor names its pass and
// the template records the matching dispatch.
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
//   static auto GetPass(frame)         -> resource&;  // pipeline + its bindings
//   static auto Prepare(frame, pc)     -> ComputePassArgs<...>;
//   static bool ShouldRun(frame);                     // optional early-out
//
// GetPass returns a resource that carries both halves of what a dispatch needs:
// a Vk::BoundComputePass bundle, or a double-buffered pass that already owns its
// heapBindings. There is no second lookup for the bindings -- a descriptor that
// had to name them separately could name a set that does not belong to the
// pipeline it just returned, and nothing would catch it.
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

        const auto args = Desc::Prepare(frame, pc);

        // Zero-extent check: if screen/viewport/particles are 0, skip pass cleanly
        if (args.extent.width == 0 || args.extent.height == 0 || args.extent.depth == 0) [[unlikely]] {
            return;
        }

        // One question: the resource answers with the pipeline and the heap rows
        // it dispatches through, whichever bundle shape it has.
        auto&      resource = Desc::GetPass(frame);
        const auto block    = std::apply(
            [&](const auto&... slots) noexcept {
                return frame.heapManager.template WriteHeapParameters<typename Desc::ShaderDecl>(Vk::TemplatedDetail::BindingsOf(resource), slots...);
            },
            args.slots
        );

        auto& pass = Vk::TemplatedDetail::PassOf(resource);
        if constexpr (TemplatedDetail::IsDoubleBufferedPass<std::remove_cvref_t<decltype(pass)>>::value) {
            // Fixed domain: the dispatch size comes from the shader, so the
            // extent is recorded but not consumed.
            if constexpr (!std::is_void_v<typename Desc::PushConstants>) {
                pass.template DispatchHeap<typename Desc::ShaderModule>(cmd, block, args.push);
            } else {
                pass.DispatchHeap(cmd, block);
            }
        } else {
            // Typed dispatch: the module's threadgroup size is a compile-time
            // constant (Vk::LocalSizeOf), so the group counts are shift math
            // and a zero extent is a no-op by construction.
            if constexpr (!std::is_void_v<typename Desc::PushConstants>) {
                pass.DispatchThreads(cmd, args.extent.width, args.extent.height, args.extent.depth, args.push, block.slot);
            } else {
                pass.DispatchThreads(cmd, args.extent.width, args.extent.height, args.extent.depth, block.slot);
            }
        }
    }
};

} // namespace ZHLN::Passes
