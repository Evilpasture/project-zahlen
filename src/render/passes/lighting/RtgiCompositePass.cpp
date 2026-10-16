// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/RtgiCompositePass.hpp"
#include "PassPayloads.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void RtgiCompositePass::operator()(VkCommandBuffer cmd) const noexcept {
    using namespace Shaders;
    frame.BindHeapsAndPushFrame(cmd);

    const auto fIdx       = frame.presenter.frameIndex;
    const bool rtgiActive = frame.ctx.RayTracingSupported() && frame.settings.rayTracing.enableDiffuseGI && frame.frames.tlasAddresses[fIdx] != nullptr;
    const auto block      = frame.heapManager.WriteHeapParameters<RtgiComposite>(
        frame.postProcess.RtgiCompositeHeapBindings(), Vk::Slot<"texLighting">(Vk::Assume<Vk::ComputeRead<Res_Lighting>>(frame.graphResources.lightingTarget)),
        Vk::Slot<"texAlbedo">(Vk::Assume<Vk::ComputeRead<Res_SceneColor>>(frame.graphResources.sceneColor)),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::ComputeRead<Res_Depth>>(frame.presenter.depthTarget)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"texAnisotropy">(Vk::Assume<Vk::ComputeRead<Res_Anisotropy>>(frame.graphResources.anisotropyBuffer)),
        Vk::Slot<"texClearcoat">(Vk::Assume<Vk::ComputeRead<Res_Clearcoat>>(frame.graphResources.clearcoatBuffer)),
        Vk::Slot<"texSheen">(Vk::Assume<Vk::ComputeRead<Res_Sheen>>(frame.graphResources.sheenBuffer)),
        Vk::Slot<"texRtgiHalf">(Vk::Assume<Vk::ComputeRead<Res_RtgiHalf>>(frame.graphResources.rtgiHalf)),
        Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"outLighting">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.lightingGI))
    );

    const RtgiCompositePushConstants push {
        .fullRes     = {frame.graphResources.lightingGI.extent.width, frame.graphResources.lightingGI.extent.height},
        .enabled     = rtgiActive ? 1u : 0u,
        .giIntensity = frame.settings.post.giIntensity,
    };
    frame.postProcess.RtgiComposite().DispatchHeapIndexedThreads<Modules::RtgiCompositeCS>(
        cmd, block, frame.graphResources.lightingGI.extent.width, frame.graphResources.lightingGI.extent.height, 1, push
    );
}

} // namespace ZHLN::Passes
