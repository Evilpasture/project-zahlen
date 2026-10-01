// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/postprocess/HdrDenoisePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// One A-Trous iteration: the step width is the iteration's half-pixel offset,
// and the two phi terms are how far apart a neighbour's depth and normal may
// be before it stops contributing.
struct HdrAtrousPushConstants {
    uint32_t stepSize;
    float    phiDepth;
    float    phiNormal;
    uint32_t _pad;
};
static_assert(GpuAbi::ScenePassPayload<HdrAtrousPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

namespace {

[[nodiscard]] auto Atrous(uint32_t stepSize) noexcept -> HdrAtrousPushConstants {
    return HdrAtrousPushConstants {.stepSize = stepSize, .phiDepth = 0.02f, .phiNormal = 16.0f, ._pad = 0u};
}

} // namespace

void HdrDenoisePass::operator()(VkCommandBuffer cmd) const noexcept {
    const uint32_t passes = impl.settings.rayTracing.denoiserPasses;
    const bool     active = impl.ctx.RayTracingSupported() && passes > 0 && (impl.settings.rayTracing.enableShadows || impl.settings.rayTracing.enableReflections);
    if (!active) {
        return;
    }
    impl.BindHeapsAndPushFrame(cmd);

    const uint32_t fIdx = impl.presenter.frameIndex;

    const auto hdr      = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.hdrSceneColor);
    const auto denoiseA = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.denoiseA);
    const auto denoiseB = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.denoiseB);
    const auto depth    = Vk::Assume<Vk::ShaderRead<Res_Depth>>(impl.presenter.depthTarget);
    const auto norm     = Vk::Assume<Vk::ShaderRead<Res_NormRough>>(impl.graphResources.normalRoughnessBuffer);

    Vk::ComputeChain atrousChain(impl.ctx, impl.heapManager, cmd);

    const auto Dispatch = [&](const auto& src, const auto& dst, uint32_t stepSize) noexcept {
        atrousChain.Step<Shaders::HdrDenoise>(
            impl.postProcess.HdrDenoise(), impl.postProcess.HdrDenoiseHeapBindings(), dst.Extent(), Atrous(stepSize),
            Vk::Slot<"inColor">(src),
            Vk::Slot<"texDepth">(depth),
            Vk::Slot<"texNormalRoughness">(norm),
            Vk::Slot<"outColor">(dst),
            Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx])
        );
    };

    switch (passes) {
        case 1:
            Dispatch(hdr, denoiseA, 1);
            Dispatch(denoiseA, hdr, 2);
            break;
        case 2:
            Dispatch(hdr, denoiseA, 1);
            Dispatch(denoiseA, denoiseB, 2);
            Dispatch(denoiseB, hdr, 2);
            break;
        default:
            Dispatch(hdr, denoiseA, 1);
            Dispatch(denoiseA, denoiseB, 2);
            Dispatch(denoiseB, hdr, 4);
            break;
    }
}

}
