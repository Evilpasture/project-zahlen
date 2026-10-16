// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/RtgiHalfTracePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

struct RtgiHalfPushConstants {
    uint32_t halfRes[2];
    uint32_t _pad[2];
};
static_assert(GpuAbi::ScenePassPayload<RtgiHalfPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void RtgiHalfTracePass::operator()(VkCommandBuffer cmd) const noexcept {
    if (!frame.ctx.RayTracingSupported() || !frame.settings.rayTracing.enableDiffuseGI || frame.settings.environment.fullBright != 0) {
        return;
    }

    const auto fIdx = frame.presenter.frameIndex;
    if (frame.frames.tlasAddresses[fIdx] == nullptr) {
        return;
    }
    frame.BindHeapsAndPushFrame(cmd);

    const Vk::AsAddressWrite tlas {.address = frame.frames.tlasAddresses[fIdx].Get()};
    const auto               block = frame.heapManager.WriteHeapParameters<Shaders::RtgiHalf>(
        frame.postProcess.RtgiHalfHeapBindings(), Vk::Slot<"texDepth">(Vk::Assume<Vk::ComputeRead<Res_Depth>>(frame.presenter.depthTarget)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"texAnisotropy">(Vk::Assume<Vk::ComputeRead<Res_Anisotropy>>(frame.graphResources.anisotropyBuffer)),
        Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]), Vk::Slot<"g_instances">(frame.frames.instanceDataBuffers[fIdx]),
        Vk::Slot<"blueNoiseTex">(frame.textureManager.ViewInfo(frame.blueNoiseTexIdx)),
        Vk::Slot<"outImage">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.rtgiHalf)), Vk::Slot<"tlas">(tlas),
        Vk::Slot<"texDdgiIrradiance">(Vk::Assume<Vk::ComputeRead<Res_DdgiIrradiance>>(frame.graphResources.ddgiIrradiance)),
        Vk::Slot<"texDdgiDistance">(Vk::Assume<Vk::ComputeRead<Res_DdgiDistance>>(frame.graphResources.ddgiDistance))
    );

    const RtgiHalfPushConstants push {.halfRes = {frame.graphResources.rtgiHalf.extent.width, frame.graphResources.rtgiHalf.extent.height}};
    frame.postProcess.RtgiHalf().DispatchHeapIndexedThreads<Shaders::Modules::RtgiHalfCS>(
        cmd, block, frame.graphResources.rtgiHalf.extent.width, frame.graphResources.rtgiHalf.extent.height, 1, push
    );
}

} // namespace ZHLN::Passes
