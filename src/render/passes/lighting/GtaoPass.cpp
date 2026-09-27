// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/GtaoPass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// Horizontal-horizon search parameters: the AO radius in world units, the
// bias and power that shape the falloff, and the matrices the search
// reconstructs view-space position with.
struct GtaoPushConstants {
    uint32_t   halfRes[2];
    float      rcpFullRes[2];
    float      time;
    float      aoRadius;
    float      aoBias;
    float      aoPower;
    uint32_t   giSamples;
    JPH::Mat44 invViewProj;
    JPH::Mat44 viewProj;
};
static_assert(GpuAbi::ScenePassPayload<GtaoPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void GtaoPass::operator()(VkCommandBuffer cmd) const noexcept {
    const int giMode = impl.settings.post.mode;
    if (giMode != 3 && giMode != 4) {
        return;
    }
    impl.BindHeapsAndPushFrame(cmd);

    const uint32_t fIdx = impl.presenter.frameIndex;

    const Vk::HeapBlockBase block = impl.heapManager.WriteHeapParameters<Shaders::Gtao>(
        impl.ctx, impl.postProcess.GtaoHeapBindings(),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_Depth>>(impl.ActivePresentation().depthTarget)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ShaderRead<Res_NormRough>>(impl.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"outAo">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.ao))
    );

    const auto& fullExt = impl.presenter.depthTarget.extent;
    const GtaoPushConstants push {
        .halfRes     = {impl.graphResources.ao.extent.width, impl.graphResources.ao.extent.height},
        .rcpFullRes  = {1.0f / static_cast<float>(fullExt.width), 1.0f / static_cast<float>(fullExt.height)},
        .time        = pc.camPos[3],
        .aoRadius    = pc.aoRadius,
        .aoBias      = pc.aoBias,
        .aoPower     = pc.aoPower,
        .giSamples   = static_cast<uint32_t>(pc.giSamples),
        .invViewProj = pc.invViewProj,
        .viewProj    = pc.viewProj,
    };
    impl.postProcess.Gtao().DispatchHeapIndexedThreads<Shaders::Modules::GtaoCS>(
        impl.ctx, cmd, block, impl.graphResources.ao.extent.width, impl.graphResources.ao.extent.height, 1, push
    );
}

}
