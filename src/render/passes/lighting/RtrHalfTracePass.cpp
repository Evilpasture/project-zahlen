// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/RtrHalfTracePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// The trace writes a half-resolution target, so the shader needs the output
// size in pixels to map a thread back to a full-resolution one.
struct RtrHalfPushConstants {
    uint32_t halfRes[2];
    uint32_t _pad[2];
};
static_assert(GpuAbi::ScenePassPayload<RtrHalfPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void RtrHalfTracePass::operator()(VkCommandBuffer cmd) const noexcept {
    if (!impl.ctx.RayTracingSupported() || !impl.settings.rayTracing.enableReflections || !impl.settings.post.enableRTR) {
        return;
    }
    impl.BindHeapsAndPushFrame(cmd);

    const uint32_t fIdx = impl.presenter.frameIndex;

    const Vk::AsAddressWrite tlas {
        .address = impl.frames.tlas[fIdx] ? Vk::GetAccelerationStructureAddress(impl.ctx.Device(), impl.frames.tlas[fIdx].Get()) : 0
    };
    const Vk::HeapBlockBase block = impl.heapManager.WriteHeapParameters<Shaders::RtrHalf>(
        impl.ctx, impl.postProcess.RtrHalfHeapBindings(),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_Depth>>(impl.presenter.depthTarget)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ShaderRead<Res_NormRough>>(impl.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"texAnisotropy">(Vk::Assume<Vk::ShaderRead<Res_Anisotropy>>(impl.graphResources.anisotropyBuffer)),
        Vk::Slot<"texLighting">(Vk::Assume<Vk::ShaderRead<Res_Lighting>>(impl.graphResources.lightingTarget)),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"g_instances">(impl.frames.instanceDataBuffers[fIdx]),
        Vk::Slot<"blueNoiseTex">(impl.textureManager.View(impl.blueNoiseTexIdx)),
        Vk::Slot<"outImage">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.rtrHalf)),
        Vk::Slot<"tlas">(tlas)
    );

    const RtrHalfPushConstants push {
        .halfRes = {impl.graphResources.rtrHalf.extent.width, impl.graphResources.rtrHalf.extent.height}, ._pad = {}
    };
    impl.postProcess.RtrHalf().DispatchHeapIndexedThreads<Shaders::Modules::RtrHalfCS>(
        impl.ctx, cmd, block, impl.graphResources.rtrHalf.extent.width, impl.graphResources.rtrHalf.extent.height, 1, push
    );
}

}
