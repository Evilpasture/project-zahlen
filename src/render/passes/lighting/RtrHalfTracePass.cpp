// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/RtrHalfTracePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

struct RtrHalfPushConstants {
    uint32_t halfRes[2];
    uint32_t _pad[2];
};
static_assert(GpuAbi::ScenePassPayload<RtrHalfPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void RtrHalfTracePass::operator()(VkCommandBuffer cmd) const noexcept {
    if (!frame.ctx.RayTracingSupported() || !frame.settings.rayTracing.enableReflections || !frame.settings.post.enableRTR) {
        return;
    }
    frame.BindHeapsAndPushFrame(cmd);

    const auto fIdx = frame.presenter.frameIndex;

    const Vk::AsAddressWrite tlas {.address = frame.frames.tlasAddresses[fIdx].Get()};
    const auto               block = frame.heapManager.WriteHeapParameters<Shaders::RtrHalf>(
        frame.postProcess.RtrHalfHeapBindings(), Vk::Slot<"texDepth">(Vk::Assume<Vk::ComputeRead<Res_Depth>>(frame.presenter.depthTarget)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"texAnisotropy">(Vk::Assume<Vk::ComputeRead<Res_Anisotropy>>(frame.graphResources.anisotropyBuffer)),

        Vk::Slot<"texLighting">(Vk::Assume<Vk::ComputeRead<Res_Lighting>>(frame.graphResources.lightingTarget)),
        Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]), Vk::Slot<"g_instances">(frame.frames.instanceDataBuffers[fIdx]),
        Vk::Slot<"blueNoiseTex">(frame.textureManager.ViewInfo(frame.blueNoiseTexIdx)),
        Vk::Slot<"outImage">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.rtrHalf)), Vk::Slot<"tlas">(tlas),
        Vk::Slot<"texDdgiIrradiance">(Vk::Assume<Vk::ComputeRead<Res_DdgiIrradiance>>(frame.graphResources.ddgiIrradiance)),
        Vk::Slot<"texDdgiDistance">(Vk::Assume<Vk::ComputeRead<Res_DdgiDistance>>(frame.graphResources.ddgiDistance))
    );

    const RtrHalfPushConstants push {.halfRes = {frame.graphResources.rtrHalf.extent.width, frame.graphResources.rtrHalf.extent.height}};
    frame.postProcess.RtrHalf().DispatchHeapIndexedThreads<Shaders::Modules::RtrHalfCS>(
        cmd, block, frame.graphResources.rtrHalf.extent.width, frame.graphResources.rtrHalf.extent.height, 1, push
    );
}

} // namespace ZHLN::Passes
