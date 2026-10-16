// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/postprocess/HdrDenoisePass.hpp"
#include "PassPayloads.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

namespace {

[[nodiscard]] auto Atrous(uint32_t stepSize) noexcept -> HdrAtrousPushConstants {
    return HdrAtrousPushConstants {.stepSize = stepSize, .phiDepth = 0.02f, .phiNormal = 16.0f, .phiLuma = 2.0f};
}

} // namespace

void HdrDenoisePass::operator()(VkCommandBuffer cmd) const noexcept {
    using namespace Shaders;
    const auto passes = frame.settings.rayTracing.denoiserPasses;

    if (!frame.ctx.RayTracingSupported() || passes <= 0 || (!frame.settings.rayTracing.enableShadows && !frame.settings.rayTracing.enableReflections)) {
        return;
    }
    frame.BindHeapsAndPushFrame(cmd);

    const auto hdr      = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.hdrSceneColor);
    const auto denoiseA = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.denoiseA);
    const auto denoiseB = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.denoiseB);
    const auto depth    = Vk::Assume<Vk::ComputeRead<Res_Depth>>(frame.ActivePresentation().depthTarget);
    const auto norm     = Vk::Assume<Vk::ComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer);

    Vk::ComputeChain atrousChain(frame.heapManager, cmd);

    const auto Dispatch = [&](const auto& src, const auto& dst, uint32_t stepSize) noexcept {
        atrousChain.Step<HdrDenoise>(
            frame.postProcess.HdrDenoise(), frame.postProcess.HdrDenoiseHeapBindings(), dst.Extent(), Atrous(stepSize), Vk::Slot<"inColor">(src),
            Vk::Slot<"texDepth">(depth), Vk::Slot<"texNormalRoughness">(norm), Vk::Slot<"outColor">(dst),
            Vk::Slot<"frame">(frame.frames.frameUniformBuffers[frame.presenter.frameIndex])
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
            Dispatch(hdr, denoiseA, 1);
            Dispatch(denoiseA, hdr, 2);
            break;
        case 2:
            Dispatch(hdr, denoiseA, 1);
            Dispatch(denoiseA, denoiseB, 2);
            Dispatch(denoiseB, hdr, 4);
            break;
        default:
            Dispatch(hdr, denoiseA, 1);
            Dispatch(denoiseA, denoiseB, 2);
            Dispatch(denoiseB, denoiseA, 4);
            Dispatch(denoiseA, hdr, 8);
            break;
    }
}

} // namespace ZHLN::Passes
