// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/ShadowDenoisePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

struct ShadowAtrousPushConstants {
    uint32_t stepSize;
    float    phiDepth;
    float    phiNormal;
    float    temporalWeight;
};
static_assert(GpuAbi::ScenePassPayload<ShadowAtrousPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

namespace {

[[nodiscard]] auto Atrous(uint32_t stepSize, float temporalWeight) noexcept -> ShadowAtrousPushConstants {
    return ShadowAtrousPushConstants {.stepSize = stepSize, .phiDepth = 0.02f, .phiNormal = 16.0f, .temporalWeight = temporalWeight};
}

} // namespace

void ShadowDenoisePass::operator()(VkCommandBuffer cmd) const noexcept {
    const uint32_t fIdx   = frame.presenter.frameIndex;
    const uint32_t passes = frame.settings.rayTracing.denoiserPasses;
    const bool     wantRT = (frame.settings.rayTracing.enableReflections && frame.settings.post.enableRTR != 0) || frame.settings.rayTracing.enableShadows;
    if (!frame.ctx.RayTracingSupported() || !wantRT || passes == 0 || frame.settings.environment.fullBright != 0 ||
        frame.frames.tlasAddresses[fIdx] == nullptr) {
        return;
    }

    frame.BindHeapsAndPushFrame(cmd);

    const auto shadowMask = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.rtShadowMask);
    const auto denoiseA   = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.shadowDenoiseA);
    const auto denoiseB   = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.shadowDenoiseB);
    const auto history    = Vk::Assume<Vk::ComputeRead<Res_RtShadowHistory>>(frame.graphResources.rtShadowHistory);
    const auto velocity   = Vk::Assume<Vk::ComputeRead<Res_Velocity>>(frame.graphResources.velocityBuffer);
    const auto depth      = Vk::Assume<Vk::ComputeRead<Res_Depth>>(frame.ActivePresentation().depthTarget);
    const auto normal     = Vk::Assume<Vk::ComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer);

    const float temporalW = frame.rtHistoryValid ? 0.85f : 0.0f;

    Vk::ComputeChain atrousChain(frame.heapManager, cmd);
    const auto       Dispatch = [&](const auto& src, const auto& dst, uint32_t stepSize, float tempWeight) noexcept {
        atrousChain.Step<Shaders::ShadowDenoise>(
            frame.postProcess.ShadowDenoise(), frame.postProcess.ShadowDenoiseHeapBindings(), dst.Extent(), Atrous(stepSize, tempWeight),
            Vk::Slot<"inShadow">(src), Vk::Slot<"texDepth">(depth), Vk::Slot<"texNormalRoughness">(normal), Vk::Slot<"texHistory">(history),
            Vk::Slot<"texVelocity">(velocity), Vk::Slot<"outShadow">(dst), Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx])
        );
    };

    // `denoiserPasses` is a quality tier, not a literal iteration count: the
    // cheapest valid configuration still needs two dispatches, because a single
    // pass would have to read and write the same image, and the frame swaps the
    // written image into the history. Tier 2 and tier 3 previously ran the same
    // three dispatches, so the top of the ladder was dead. Tier 3 now adds the
    // step-8 pass; tier 2 is unchanged and stays the default.
    switch (passes) {
        case 1:
            Dispatch(shadowMask, denoiseA, 1, temporalW);
            Dispatch(denoiseA, shadowMask, 2, 0.0f);
            break;
        case 2:
            Dispatch(shadowMask, denoiseA, 1, temporalW);
            Dispatch(denoiseA, denoiseB, 2, 0.0f);
            Dispatch(denoiseB, shadowMask, 4, 0.0f);
            break;
        default:
            Dispatch(shadowMask, denoiseA, 1, temporalW);
            Dispatch(denoiseA, denoiseB, 2, 0.0f);
            Dispatch(denoiseB, denoiseA, 4, 0.0f);
            Dispatch(denoiseA, shadowMask, 8, 0.0f);
            break;
    }
}

} // namespace ZHLN::Passes
