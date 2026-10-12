// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/RtgiDenoisePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

struct RtgiAtrousPushConstants {
    uint32_t stepSize;
    float    phiDepth;
    float    phiNormal;
    float    phiLuma;
    float    temporalWeight;
    float    maxRoughness;
    uint32_t _pad[2];
};
static_assert(GpuAbi::ScenePassPayload<RtgiAtrousPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

namespace {

[[nodiscard]] auto Atrous(uint32_t stepSize, float temporalWeight) noexcept -> RtgiAtrousPushConstants {
    return RtgiAtrousPushConstants {
        .stepSize       = stepSize,
        .phiDepth       = 0.02f,
        .phiNormal      = 16.0f,
        .phiLuma        = 2.0f,
        .temporalWeight = temporalWeight,
        .maxRoughness   = 1.0f,
        ._pad           = {0u, 0u},
    };
}

} // namespace

void RtgiDenoisePass::operator()(VkCommandBuffer cmd) const noexcept {
    const uint32_t fIdx   = impl.presenter.frameIndex;
    const uint32_t passes = impl.settings.rayTracing.denoiserPasses;
    if (!impl.ctx.RayTracingSupported() || !impl.settings.rayTracing.enableDiffuseGI || passes == 0 || impl.settings.environment.fullBright != 0 ||
        !impl.frames.tlas[fIdx]) {
        return;
    }

    impl.BindHeapsAndPushFrame(cmd);

    const auto rtgiHalf = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.rtgiHalf);
    const auto denoiseA = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.rtgiDenoiseA);
    const auto denoiseB = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.rtgiDenoiseB);
    const auto history  = Vk::Assume<Vk::ComputeRead<Res_RtgiHistory>>(impl.graphResources.rtgiHistory);
    const auto velocity = Vk::Assume<Vk::ComputeRead<Res_Velocity>>(impl.graphResources.velocityBuffer);
    const auto depth    = Vk::Assume<Vk::ComputeRead<Res_Depth>>(impl.ActivePresentation().depthTarget);
    const auto normal   = Vk::Assume<Vk::ComputeRead<Res_NormRough>>(impl.graphResources.normalRoughnessBuffer);

    const float temporalW = impl.rtHistoryValid ? 0.85f : 0.0f;

    Vk::ComputeChain atrousChain(impl.ctx, impl.heapManager, cmd);
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

    switch (passes) {
        case 1:
            Dispatch(rtgiHalf, denoiseA, 1, temporalW);
            Dispatch(denoiseA, rtgiHalf, 2, 0.0f);
            break;
        default:
            Dispatch(rtgiHalf, denoiseA, 1, temporalW);
            Dispatch(denoiseA, denoiseB, 2, 0.0f);
            Dispatch(denoiseB, rtgiHalf, 4, 0.0f);
            break;
    }
}

}
