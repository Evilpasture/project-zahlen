// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/RtgiCompositePass.hpp"
#include "PassPayloads.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void RtgiCompositePass::operator()(VkCommandBuffer cmd) const noexcept {
    using namespace Shaders;

    const auto fIdx       = frame.presenter.frameIndex;
    const bool rtgiActive = frame.ctx.RayTracingSupported() && frame.settings.rayTracing.enableDiffuseGI && frame.frames.tlasAddresses[fIdx] != nullptr;
    const RtgiCompositePushConstants push {
        .fullRes     = {frame.graphResources.lightingGI.extent.width, frame.graphResources.lightingGI.extent.height},
        .enabled     = rtgiActive ? 1u : 0u,
        .giIntensity = frame.settings.post.giIntensity,
    };

    // The bundle writes its own heap rows and dispatches: no separate bindings
    // lookup, no separate pipeline lookup.
    frame.lighting.rtgiComposite.DispatchIndexed<RtgiComposite>(
        cmd, frame.heapManager, frame.graphResources.lightingGI.extent.width, frame.graphResources.lightingGI.extent.height, 1, push,
        Vk::Slot<"texLighting">(Vk::Assume<Vk::ComputeRead<Res_Lighting>>(frame.graphResources.lightingTarget)),
        Vk::Slot<"texLightingAo">(Vk::Assume<Vk::ComputeRead<Res_LightingAo>>(frame.graphResources.lightingAo)),
        Vk::Slot<"texAlbedo">(Vk::Assume<Vk::ComputeRead<Res_SceneColor>>(frame.graphResources.sceneColor)),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::ComputeRead<Res_Depth>>(frame.ActiveDepthTarget())),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"texRtgiHalf">(Vk::Assume<Vk::ComputeRead<Res_RtgiHalf>>(frame.graphResources.rtgiHalf)),
        Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"outLighting">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.lightingGI))
    );
}

} // namespace ZHLN::Passes
