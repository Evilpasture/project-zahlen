// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/RtgiDenoisePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

struct RtgiAtrousPushConstants {
    uint32_t stepSize;
    float    phiDepth;
    float    phiNormal;
    uint32_t _pad;
};
static_assert(GpuAbi::ScenePassPayload<RtgiAtrousPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

namespace {

[[nodiscard]] auto Atrous(uint32_t stepSize) noexcept -> RtgiAtrousPushConstants {
    return RtgiAtrousPushConstants {.stepSize = stepSize, .phiDepth = 0.02f, .phiNormal = 16.0f, ._pad = 0u};
}

} // namespace

void RtgiDenoisePass::operator()(VkCommandBuffer cmd) const noexcept {
    const uint32_t fIdx = impl.presenter.frameIndex;
    const uint32_t passes = impl.settings.rayTracing.denoiserPasses;
    if (!impl.ctx.RayTracingSupported() || !impl.settings.rayTracing.enableDiffuseGI || passes == 0 || impl.settings.environment.fullBright != 0 ||
        !impl.frames.tlas[fIdx]) {
        return;
    }

    impl.BindHeapsAndPushFrame(cmd);

    const auto rtgiHalf  = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.rtgiHalf);
    const auto denoiseA  = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.rtgiDenoiseA);
    const auto denoiseB  = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.rtgiDenoiseB);
    const auto depth     = Vk::Assume<Vk::ShaderRead<Res_Depth>>(impl.presenter.depthTarget);
    const auto normal    = Vk::Assume<Vk::ShaderRead<Res_NormRough>>(impl.graphResources.normalRoughnessBuffer);

    Vk::ComputeChain atrousChain(impl.ctx, impl.heapManager, cmd);
    const auto Dispatch = [&](const auto& src, const auto& dst, uint32_t stepSize) noexcept {
        atrousChain.Step<Shaders::RtgiDenoise>(
            impl.postProcess.RtgiDenoise(), impl.postProcess.RtgiDenoiseHeapBindings(), dst.Extent(), Atrous(stepSize),
            Vk::Slot<"inColor">(src),
            Vk::Slot<"texDepth">(depth),
            Vk::Slot<"texNormalRoughness">(normal),
            Vk::Slot<"outColor">(dst),
            Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx])
        );
    };

    switch (passes) {
        case 1:
            Dispatch(rtgiHalf, denoiseA, 1);
            Dispatch(denoiseA, rtgiHalf, 2);
            break;
        case 2:
            Dispatch(rtgiHalf, denoiseA, 1);
            Dispatch(denoiseA, denoiseB, 2);
            Dispatch(denoiseB, rtgiHalf, 2);
            break;
        default:
            Dispatch(rtgiHalf, denoiseA, 1);
            Dispatch(denoiseA, denoiseB, 2);
            Dispatch(denoiseB, rtgiHalf, 4);
            break;
    }
}

}
