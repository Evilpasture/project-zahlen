// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/RtShadowPass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

struct RTShadowPushConstants {
    uint32_t samples;
    float    normalBias;
    float    maxDistance;
    uint32_t _pad;
};
static_assert(GpuAbi::ScenePassPayload<RTShadowPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void RtShadowPass::operator()(VkCommandBuffer cmd) const noexcept {
    const uint32_t fIdx   = impl.presenter.frameIndex;
    const bool     wantRT = (impl.settings.rayTracing.enableReflections && impl.settings.post.enableRTR != 0) || impl.settings.rayTracing.enableShadows;
    if (!impl.ctx.RayTracingSupported() || !wantRT || impl.settings.environment.fullBright != 0 || impl.frames.tlasAddresses[fIdx] == nullptr) {
        return;
    }

    impl.BindHeapsAndPushFrame(cmd);

    const Vk::AsAddressWrite tlas {
        .address = impl.frames.tlasAddresses[fIdx].Get()
    };
    const Vk::HeapBlockBase block = impl.heapManager.WriteHeapParameters<Shaders::RtShadow>(
        impl.postProcess.RtShadowHeapBindings(),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::FragmentAndComputeRead<Res_Depth>>(impl.ActivePresentation().depthTarget)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::FragmentAndComputeRead<Res_NormRough>>(impl.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"outShadow">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.rtShadowMask)),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"tlas">(tlas),
        Vk::Slot<"blueNoiseTex">(impl.textureManager.ViewInfo(impl.blueNoiseTexIdx))
    );

    const RTShadowPushConstants push {
        .samples     = 1u,
        .normalBias  = 0.04f,
        .maxDistance = 300.0f,
        ._pad        = 0u,
    };
    impl.postProcess.RtShadow().DispatchHeapIndexedThreads<Shaders::Modules::RtShadowCS>(
        cmd, block, impl.graphResources.rtShadowMask.extent.width, impl.graphResources.rtShadowMask.extent.height, 1, push
    );
}

}
