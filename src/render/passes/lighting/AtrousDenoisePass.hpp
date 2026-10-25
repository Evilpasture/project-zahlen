// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "PassPayloads.hpp"
#include "TargetManager.hpp"
#include "graph/RenderGraph.hpp"
#include "passes/GraphPassUtils.hpp"
#include <ShaderBindings.hpp>
#include <concepts>
#include <type_traits>

namespace ZHLN::Passes {

// The one push-constant filler shared by every à-trous chain. The first
// dispatch carries the temporal weight (and the roughness ceiling where the
// payload has one); the follow-up hops never reproject.
//
// phiLuma is a Karis-space tolerance: the wavelet shaders compare
// KarisLuma = y/(1+y), which lives in [0, 1), so a luma difference of 0.05 is
// already a strong edge and 0.015 is noise. The pre-fix value (2.0f, sensible
// only against uncompressed HDR luma) never let the luma weight fall below
// e^-0.5, so the one term meant to protect texture detail was decorative and
// the filter blurred every continuous surface -- "the whole frame is soft".
// phiDepth stays relative to view depth (2% per plane), phiNormal is the
// normal-dot exponent.
template <typename PushT>
[[nodiscard]] constexpr auto MakeAtrousPush(uint32_t stepSize) noexcept -> PushT {
    static_assert(std::same_as<PushT, HdrAtrousPushConstants>, "an à-trous push payload MakeAtrousPush does not know how to fill");
    // The composed frame this chain filters is already deterministic upstream;
    // its luma structure is detail, not noise, so the tolerance is tight.
    return HdrAtrousPushConstants {.stepSize = stepSize, .phiDepth = 0.02f, .phiNormal = 16.0f, .phiLuma = 0.05f};
}

// The HDR wavelet deliberately ignores fullBright and the TLAS state (it
// filters whatever landed in the scene color while bypassing sharp/metallic
// Tier 1 specular pixels in shader).
struct HdrDenoiseGate {
    [[nodiscard]] static bool ShouldRun(RenderFrameContext& frame) noexcept {
        return frame.ctx.RayTracingSupported() && frame.settings.rayTracing.denoiserPasses > 0 &&
               (frame.settings.rayTracing.enableShadows || frame.settings.rayTracing.enableReflections);
    }
};

namespace TemplatedDetail {

// Local pointer selection for the generic denoiser. Graph binding itself
// uses the target's tag directly and does not consult this table.
template <typename Res>
struct GraphMember;

template <>
struct GraphMember<Res_HdrSceneColor> {
    static constexpr auto Ptr = &TargetManager::GraphResources::hdrSceneColor;
};
template <>
struct GraphMember<Res_DenoiseA> {
    static constexpr auto Ptr = &TargetManager::GraphResources::denoiseA;
};
template <>
struct GraphMember<Res_DenoiseB> {
    static constexpr auto Ptr = &TargetManager::GraphResources::denoiseB;
};

// Maps a descriptor set to the resource bundle that owns it.
template <typename Declared>
struct DenoiseAccess;

template <>
struct DenoiseAccess<Shaders::HdrDenoise> {
    [[nodiscard]] static auto Resource(RenderFrameContext& frame) noexcept -> Vk::BoundComputePass<Vk::DynamicComputePass>& {
        return frame.post.hdrDenoise;
    }
};

} // namespace TemplatedDetail

template <typename Declared>
[[nodiscard]] auto GetResource(RenderFrameContext& frame) noexcept -> Vk::BoundComputePass<Vk::DynamicComputePass>& {
    return TemplatedDetail::DenoiseAccess<Declared>::Resource(frame);
}

// One à-trous wavelet chain for every denoiser the frame graph runs. Each
// instantiation reads the same depth/normal taps and ping-pongs the same way;
// the parameters that differ are the set it dispatches, the target and the two
// scratch images, the temporal history it reprojects through (none for the HDR
// wavelet), the push payload, and the early-out. The shadow chain's shader
// spells its image pair inShadow/outShadow; the other shaders use
// inColor/outColor, which the payload type selects.
template <
    Vk::ResourceName PassName,
    typename ShaderDecl,
    typename TargetRes,
    typename PingRes,
    typename PongRes,
    typename HistoryRes = void,
    typename PushT      = HdrAtrousPushConstants,
    typename Gate       = HdrDenoiseGate>
struct AtrousDenoisePass:
    TemplatedDetail::RenderPassOf<
        PassName,
        typename TemplatedDetail::Concat<
            Vk::TypeList<Vk::ComputeWrite<TargetRes>, Vk::ComputeWrite<PingRes>, Vk::ComputeWrite<PongRes>>,
            std::conditional_t<std::is_void_v<HistoryRes>, Vk::TypeList<>, Vk::TypeList<Vk::ComputeRead<HistoryRes>, Vk::ComputeRead<Res_Velocity>>>,
            Vk::TypeList<Vk::ComputeRead<Res_Depth>, Vk::ComputeRead<Res_NormRough>>>::type> {
    static_assert(TemplatedDetail::kNameProfilesAsStage<PassName>, "the pass name has no Stage enumerator, so the GPU profiler would never time it");

    RenderFrameContext& frame;

    void operator()(VkCommandBuffer cmd) const noexcept {
        if (!Gate::ShouldRun(frame)) {
            return;
        }

        const uint32_t fIdx   = frame.presenter.frameIndex;
        const uint32_t passes = frame.settings.rayTracing.denoiserPasses;

        const auto target   = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.*(TemplatedDetail::GraphMember<TargetRes>::Ptr));
        const auto denoiseA = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.*(TemplatedDetail::GraphMember<PingRes>::Ptr));
        const auto denoiseB = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.*(TemplatedDetail::GraphMember<PongRes>::Ptr));
        const auto depth    = Vk::Assume<Vk::ComputeRead<Res_Depth>>(frame.ActiveDepthTarget());
        const auto normal   = Vk::Assume<Vk::ComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer);

        Vk::ComputeChain atrousChain(frame.heapManager, cmd);
        const auto       Dispatch = [&](const auto& src, const auto& dst, uint32_t stepSize) noexcept {
            const auto push = MakeAtrousPush<PushT>(stepSize);
            const auto Step = [&](auto&&... slots) noexcept {
                atrousChain.template Step<ShaderDecl>(GetResource<ShaderDecl>(frame), dst.Extent(), push, slots...);
            };

            if constexpr (!std::is_void_v<HistoryRes>) {
                const auto history  = Vk::Assume<Vk::ComputeRead<HistoryRes>>(frame.graphResources.*(TemplatedDetail::GraphMember<HistoryRes>::Ptr));
                const auto velocity = Vk::Assume<Vk::ComputeRead<Res_Velocity>>(frame.graphResources.velocityBuffer);
                Step(
                    Vk::Slot<"inColor">(src), Vk::Slot<"texDepth">(depth), Vk::Slot<"texNormalRoughness">(normal), Vk::Slot<"texHistory">(history),
                    Vk::Slot<"texVelocity">(velocity), Vk::Slot<"outColor">(dst), Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx])
                );
            } else {
                Step(
                    Vk::Slot<"inColor">(src), Vk::Slot<"texDepth">(depth), Vk::Slot<"texNormalRoughness">(normal), Vk::Slot<"outColor">(dst),
                    Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx])
                );
            }
        };

        // `denoiserPasses` is a quality tier, not a literal iteration count: the
        // cheapest valid configuration still needs two dispatches, because a single
        // pass would have to read and write the same image. Tier 3 adds the step-8
        // pass; tier 2 stays the default.
        switch (passes) {
            case 1:
                Dispatch(target, denoiseA, 1);
                Dispatch(denoiseA, target, 2);
                break;
            case 2:
                Dispatch(target, denoiseA, 1);
                Dispatch(denoiseA, denoiseB, 2);
                Dispatch(denoiseB, target, 4);
                break;
            default:
                Dispatch(target, denoiseA, 1);
                Dispatch(denoiseA, denoiseB, 2);
                Dispatch(denoiseB, denoiseA, 4);
                Dispatch(denoiseA, target, 8);
                break;
        }
    }
};

} // namespace ZHLN::Passes
