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
    const uint32_t fIdx   = impl.presenter.frameIndex;
    const uint32_t passes = impl.settings.rayTracing.denoiserPasses;
    const bool     wantRT = (impl.settings.rayTracing.enableReflections && impl.settings.post.enableRTR != 0) || impl.settings.rayTracing.enableShadows;
    if (!impl.ctx.RayTracingSupported() || !wantRT || passes == 0 || impl.settings.environment.fullBright != 0 || impl.frames.tlasAddresses[fIdx] == nullptr) {
        return;
    }

    impl.BindHeapsAndPushFrame(cmd);

    const auto shadowMask = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.rtShadowMask);
    const auto denoiseA   = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.shadowDenoiseA);
    const auto denoiseB   = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.shadowDenoiseB);
    const auto history    = Vk::Assume<Vk::ComputeRead<Res_RtShadowHistory>>(impl.graphResources.rtShadowHistory);
    const auto velocity   = Vk::Assume<Vk::ComputeRead<Res_Velocity>>(impl.graphResources.velocityBuffer);
    const auto depth      = Vk::Assume<Vk::ComputeRead<Res_Depth>>(impl.ActivePresentation().depthTarget);
    const auto normal     = Vk::Assume<Vk::ComputeRead<Res_NormRough>>(impl.graphResources.normalRoughnessBuffer);

    const float temporalW = impl.rtHistoryValid ? 0.85f : 0.0f;

    Vk::ComputeChain atrousChain(impl.heapManager, cmd);
    const auto Dispatch = [&](const auto& src, const auto& dst, uint32_t stepSize, float tempWeight) noexcept {
        atrousChain.Step<Shaders::ShadowDenoise>(
            impl.postProcess.ShadowDenoise(), impl.postProcess.ShadowDenoiseHeapBindings(), dst.Extent(), Atrous(stepSize, tempWeight),
            Vk::Slot<"inShadow">(src),
            Vk::Slot<"texDepth">(depth),
            Vk::Slot<"texNormalRoughness">(normal),
            Vk::Slot<"texHistory">(history),
            Vk::Slot<"texVelocity">(velocity),
            Vk::Slot<"outShadow">(dst),
            Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx])
        );
    };

    switch (passes) {
        case 1:
            Dispatch(shadowMask, denoiseA, 1, temporalW);
            Dispatch(denoiseA, shadowMask, 2, 0.0f);
            break;
        default:
            Dispatch(shadowMask, denoiseA, 1, temporalW);
            Dispatch(denoiseA, denoiseB, 2, 0.0f);
            Dispatch(denoiseB, shadowMask, 4, 0.0f);
            break;
    }
}

}
