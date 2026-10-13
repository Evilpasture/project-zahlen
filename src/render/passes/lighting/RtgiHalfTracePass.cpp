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
    if (!impl.ctx.RayTracingSupported() || !impl.settings.rayTracing.enableDiffuseGI || impl.settings.environment.fullBright != 0) {
        return;
    }

    const uint32_t fIdx = impl.presenter.frameIndex;
    if (!impl.frames.tlas[fIdx]) {
        return;
    }
    impl.BindHeapsAndPushFrame(cmd);

    const Vk::AsAddressWrite tlas {
        .address = Vk::GetAccelerationStructureAddress(impl.ctx.Device(), impl.frames.tlas[fIdx].Get())
    };
    const Vk::HeapBlockBase block = impl.heapManager.WriteHeapParameters<Shaders::RtgiHalf>(
        impl.postProcess.RtgiHalfHeapBindings(),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::ComputeRead<Res_Depth>>(impl.presenter.depthTarget)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ComputeRead<Res_NormRough>>(impl.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"texAnisotropy">(Vk::Assume<Vk::ComputeRead<Res_Anisotropy>>(impl.graphResources.anisotropyBuffer)),
        Vk::Slot<"texLighting">(Vk::Assume<Vk::ComputeRead<Res_Lighting>>(impl.graphResources.lightingTarget)),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"g_instances">(impl.frames.instanceDataBuffers[fIdx]),
        Vk::Slot<"blueNoiseTex">(impl.textureManager.ViewInfo(impl.blueNoiseTexIdx)),
        Vk::Slot<"outImage">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.rtgiHalf)),
        Vk::Slot<"tlas">(tlas),
        Vk::Slot<"texDdgiIrradiance">(Vk::Assume<Vk::ComputeRead<Res_DdgiIrradiance>>(impl.graphResources.ddgiIrradiance)),
        Vk::Slot<"texDdgiDistance">(Vk::Assume<Vk::ComputeRead<Res_DdgiDistance>>(impl.graphResources.ddgiDistance))
    );

    const RtgiHalfPushConstants push {
        .halfRes = {impl.graphResources.rtgiHalf.extent.width, impl.graphResources.rtgiHalf.extent.height}, ._pad = {}
    };
    impl.postProcess.RtgiHalf().DispatchHeapIndexedThreads<Shaders::Modules::RtgiHalfCS>(
        cmd, block, impl.graphResources.rtgiHalf.extent.width, impl.graphResources.rtgiHalf.extent.height, 1, push
    );
}

}
