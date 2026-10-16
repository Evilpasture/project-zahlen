// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/RtShadowPass.hpp"
#include "PassPayloads.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void RtShadowPass::operator()(VkCommandBuffer cmd) const noexcept {
    using namespace Shaders;
    const auto fIdx   = frame.presenter.frameIndex;
    const bool wantRT = (frame.settings.rayTracing.enableReflections && frame.settings.post.enableRTR != 0) || frame.settings.rayTracing.enableShadows;
    if (!frame.ctx.RayTracingSupported() || !wantRT || frame.settings.environment.fullBright != 0 || frame.frames.tlasAddresses[fIdx] == nullptr) {
        return;
    }

    frame.BindHeapsAndPushFrame(cmd);

    const Vk::AsAddressWrite tlas {.address = frame.frames.tlasAddresses[fIdx].Get()};
    const auto               block = frame.heapManager.WriteHeapParameters<RtShadow>(
        frame.postProcess.RtShadowHeapBindings(),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::FragmentAndComputeRead<Res_Depth>>(frame.ActivePresentation().depthTarget)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::FragmentAndComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"outShadow">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.rtShadowMask)),
        Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]), Vk::Slot<"tlas">(tlas),
        Vk::Slot<"blueNoiseTex">(frame.textureManager.ViewInfo(frame.blueNoiseTexIdx))
    );

    const RTShadowPushConstants push {
        .samples     = 1u,
        .normalBias  = 0.04f,
        .maxDistance = 300.0f,
    };
    frame.postProcess.RtShadow().DispatchHeapIndexedThreads<Modules::RtShadowCS>(
        cmd, block, frame.graphResources.rtShadowMask.extent.width, frame.graphResources.rtShadowMask.extent.height, 1, push
    );
}

} // namespace ZHLN::Passes
