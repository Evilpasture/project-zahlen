// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/RtrDenoisePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

namespace {

[[nodiscard]] auto Atrous(uint32_t stepSize, float temporalWeight) noexcept -> RtgiAtrousPushConstants {
    return RtgiAtrousPushConstants {
        .stepSize       = stepSize,
        .phiDepth       = 0.02f,
        .phiNormal      = 16.0f,
        .phiLuma        = 2.0f,
        .temporalWeight = temporalWeight,
        .maxRoughness   = 0.4f,
        ._pad           = {0u, 0u},
    };
}

} // namespace

void RtrDenoisePass::operator()(VkCommandBuffer cmd) const noexcept {
    const uint32_t fIdx   = impl.presenter.frameIndex;
    const uint32_t passes = impl.settings.rayTracing.denoiserPasses;
    if (!impl.ctx.RayTracingSupported() || !impl.settings.rayTracing.enableReflections || !impl.settings.post.enableRTR || passes == 0 ||
        impl.settings.environment.fullBright != 0 || impl.frames.tlasAddresses[fIdx] == nullptr) {
        return;
    }

    impl.BindHeapsAndPushFrame(cmd);

    const auto rtrHalf  = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.rtrHalf);
    const auto denoiseA = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.rtgiDenoiseA);
    const auto denoiseB = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.rtgiDenoiseB);
    const auto history  = Vk::Assume<Vk::ComputeRead<Res_RtrHistory>>(impl.graphResources.rtrHistory);
    const auto velocity = Vk::Assume<Vk::ComputeRead<Res_Velocity>>(impl.graphResources.velocityBuffer);
    const auto depth    = Vk::Assume<Vk::ComputeRead<Res_Depth>>(impl.ActivePresentation().depthTarget);
    const auto normal   = Vk::Assume<Vk::ComputeRead<Res_NormRough>>(impl.graphResources.normalRoughnessBuffer);

    const float temporalW = impl.rtHistoryValid ? 0.85f : 0.0f;

    Vk::ComputeChain atrousChain(impl.heapManager, cmd);
    const auto Dispatch = [&](const auto& src, const auto& dst, uint32_t stepSize, float tempWeight) noexcept {
        atrousChain.Step<Shaders::RtgiDenoise>(
            impl.postProcess.RtgiDenoise(), impl.postProcess.RtgiDenoiseHeapBindings(), dst.Extent(), Atrous(stepSize, tempWeight),
            Vk::Slot<"inColor">(src),
            Vk::Slot<"texDepth">(depth),
            Vk::Slot<"texNormalRoughness">(normal),
            Vk::Slot<"texHistory">(history),
            Vk::Slot<"texVelocity">(velocity),
            Vk::Slot<"outColor">(dst),
            Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx])
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
            Dispatch(rtrHalf, denoiseA, 1, temporalW);
            Dispatch(denoiseA, rtrHalf, 2, 0.0f);
            break;
        case 2:
            Dispatch(rtrHalf, denoiseA, 1, temporalW);
            Dispatch(denoiseA, denoiseB, 2, 0.0f);
            Dispatch(denoiseB, rtrHalf, 4, 0.0f);
            break;
        default:
            Dispatch(rtrHalf, denoiseA, 1, temporalW);
            Dispatch(denoiseA, denoiseB, 2, 0.0f);
            Dispatch(denoiseB, denoiseA, 4, 0.0f);
            Dispatch(denoiseA, rtrHalf, 8, 0.0f);
            break;
    }
}

}
