// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/GtaoPass.hpp"
#include "PassPayloads.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void GtaoPass::operator()(VkCommandBuffer cmd) const noexcept {
    const int giMode = frame.settings.post.mode;
    if (giMode != 3 && giMode != 4) {
        return;
    }
    frame.BindHeapsAndPushFrame(cmd);

    const uint32_t fIdx = frame.presenter.frameIndex;

    const Vk::HeapBlockBase block = frame.heapManager.WriteHeapParameters<Shaders::Gtao>(
        frame.postProcess.GtaoHeapBindings(), Vk::Slot<"texDepth">(Vk::Assume<Vk::FragmentAndComputeRead<Res_Depth>>(frame.ActivePresentation().depthTarget)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::FragmentAndComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]), Vk::Slot<"outAo">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.ao))
    );

    const auto&             fullExt = frame.presenter.depthTarget.extent;
    const GtaoPushConstants push {
        .halfRes     = {frame.graphResources.ao.extent.width, frame.graphResources.ao.extent.height},
        .rcpFullRes  = {1.0f / static_cast<float>(fullExt.width), 1.0f / static_cast<float>(fullExt.height)},
        .time        = pc.camPos.w,
        .aoRadius    = pc.aoRadius,
        .aoBias      = pc.aoBias,
        .aoPower     = pc.aoPower,
        .giSamples   = static_cast<uint32_t>(pc.giSamples),
        .invViewProj = pc.invViewProj,
        .viewProj    = pc.viewProj,
    };
    frame.postProcess.Gtao().DispatchHeapIndexedThreads<Shaders::Modules::GtaoCS>(
        cmd, block, frame.graphResources.ao.extent.width, frame.graphResources.ao.extent.height, 1, push
    );
}

} // namespace ZHLN::Passes
