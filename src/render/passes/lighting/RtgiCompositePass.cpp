// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/RtgiCompositePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

struct RtgiCompositePushConstants {
    uint32_t fullRes[2];
    uint32_t enabled;
    float    giIntensity;
};
static_assert(GpuAbi::ScenePassPayload<RtgiCompositePushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void RtgiCompositePass::operator()(VkCommandBuffer cmd) const noexcept {
    impl.BindHeapsAndPushFrame(cmd);

    const uint32_t fIdx = impl.presenter.frameIndex;
    const bool rtgiActive = impl.ctx.RayTracingSupported() && impl.settings.rayTracing.enableDiffuseGI && static_cast<bool>(impl.frames.tlas[fIdx]);
    const Vk::HeapBlockBase block = impl.heapManager.WriteHeapParameters<Shaders::RtgiComposite>(
        impl.postProcess.RtgiCompositeHeapBindings(),
        Vk::Slot<"texLighting">(Vk::Assume<Vk::ComputeRead<Res_Lighting>>(impl.graphResources.lightingTarget)),
        Vk::Slot<"texAlbedo">(Vk::Assume<Vk::ComputeRead<Res_SceneColor>>(impl.graphResources.sceneColor)),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::ComputeRead<Res_Depth>>(impl.presenter.depthTarget)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ComputeRead<Res_NormRough>>(impl.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"texAnisotropy">(Vk::Assume<Vk::ComputeRead<Res_Anisotropy>>(impl.graphResources.anisotropyBuffer)),
        Vk::Slot<"texClearcoat">(Vk::Assume<Vk::ComputeRead<Res_Clearcoat>>(impl.graphResources.clearcoatBuffer)),
        Vk::Slot<"texSheen">(Vk::Assume<Vk::ComputeRead<Res_Sheen>>(impl.graphResources.sheenBuffer)),
        Vk::Slot<"texRtgiHalf">(Vk::Assume<Vk::ComputeRead<Res_RtgiHalf>>(impl.graphResources.rtgiHalf)),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"outLighting">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.lightingGI))
    );

    const RtgiCompositePushConstants push {
        .fullRes = {impl.graphResources.lightingGI.extent.width, impl.graphResources.lightingGI.extent.height},
        .enabled = rtgiActive ? 1u : 0u,
        .giIntensity = impl.settings.post.giIntensity,
    };
    impl.postProcess.RtgiComposite().DispatchHeapIndexedThreads<Shaders::Modules::RtgiCompositeCS>(
        cmd, block, impl.graphResources.lightingGI.extent.width, impl.graphResources.lightingGI.extent.height, 1, push
    );
}

}
