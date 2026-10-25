// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/RtgiCompositePass.hpp"
#include "PassPayloads.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void RtgiCompositePass::operator()(VkCommandBuffer cmd) const noexcept {
    using namespace Shaders;

    const auto fIdx = frame.presenter.frameIndex;
    const bool rcgiActive =
        frame.ctx.RayTracingSupported() && frame.settings.rayTracing.enableDiffuseGI && frame.settings.post.giIntensity > 0.0f &&
        frame.settings.environment.fullBright == 0 && frame.frames.tlasAddresses[fIdx] != nullptr;

    const VkExtent2D fullExt    = frame.graphResources.lightingGI.extent;
    const VkExtent2D alignedExt = frame.graphResources.rcgiCascadeA.extent;

    const RcgiResolvePushConstants push {
        .fullRes              = {fullExt.width, fullExt.height},
        .alignedRes           = {alignedExt.width, alignedExt.height},
        .giIntensity          = rcgiActive ? frame.settings.post.giIntensity : 0.0f,
        .cMinus1Radius        = 0.25f,
        .depthRejectScale     = 1.0f,
        .skyOcclusionStrength = 0.20f,
    };

    frame.lighting.rcgiResolve.DispatchIndexed<RcgiResolve>(
        cmd, frame.heapManager, fullExt.width, fullExt.height, 1, push,
        Vk::Slot<"texLighting">(Vk::Assume<Vk::ComputeRead<Res_Lighting>>(frame.graphResources.lightingTarget)),
        Vk::Slot<"texLightingAo">(Vk::Assume<Vk::ComputeRead<Res_LightingAo>>(frame.graphResources.lightingAo)),
        Vk::Slot<"texAlbedo">(Vk::Assume<Vk::ComputeRead<Res_SceneColor>>(frame.graphResources.sceneColor)),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::ComputeRead<Res_Depth>>(frame.ActiveDepthTarget())),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"inProbes">(Vk::Assume<Vk::ComputeRead<Res_RcgiProbes>>(frame.graphResources.rcgiProbes)),
        Vk::Slot<"inCascade0">(Vk::Assume<Vk::ComputeRead<Res_RcgiCascadeA>>(frame.graphResources.rcgiCascadeA)),
        Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"outLighting">(Vk::ImageWrite {frame.graphResources.lightingGI.image.View(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1), VK_IMAGE_LAYOUT_GENERAL})
    );
}

} // namespace ZHLN::Passes
