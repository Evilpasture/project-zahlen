// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/DdgiRayTracePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

struct DdgiTracePushConstants {
    uint32_t totalProbes;
    uint32_t raysPerProbe;
    float    maxRayDistance;
    uint32_t _pad;
};
static_assert(GpuAbi::ScenePassPayload<DdgiTracePushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void DdgiRayTracePass::operator()(VkCommandBuffer cmd) const noexcept {
    const uint32_t fIdx   = impl.presenter.frameIndex;
    const bool     wantRT = impl.settings.rayTracing.enableDiffuseGI ||
                            (impl.settings.rayTracing.enableReflections && impl.settings.post.enableRTR != 0) ||
                            impl.settings.rayTracing.enableShadows;
    if (!impl.ctx.RayTracingSupported() || !wantRT || impl.settings.environment.fullBright != 0 || !impl.frames.tlas[fIdx]) {
        return;
    }

    impl.BindHeapsAndPushFrame(cmd);

    const Vk::AsAddressWrite tlas {
        .address = Vk::GetAccelerationStructureAddress(impl.ctx.Device(), impl.frames.tlas[fIdx].Get())
    };
    const Vk::HeapBlockBase block = impl.heapManager.WriteHeapParameters<Shaders::DdgiTrace>(
        impl.ctx, impl.postProcess.DdgiTraceHeapBindings(),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"g_instances">(impl.frames.instanceDataBuffers[fIdx]),
        Vk::Slot<"texDdgiIrradiance">(Vk::Assume<Vk::ComputeRead<Res_DdgiIrradiance>>(impl.graphResources.ddgiIrradiance)),
        Vk::Slot<"texDdgiDistance">(Vk::Assume<Vk::ComputeRead<Res_DdgiDistance>>(impl.graphResources.ddgiDistance)),
        Vk::Slot<"outRays">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.ddgiRays)),
        Vk::Slot<"tlas">(tlas)
    );

    const uint32_t totalProbes  = impl.graphResources.ddgiRays.extent.width;
    const uint32_t raysPerProbe = impl.graphResources.ddgiRays.extent.height;

    const DdgiTracePushConstants push {
        .totalProbes    = totalProbes,
        .raysPerProbe   = raysPerProbe,
        .maxRayDistance = 24.0f,
        ._pad           = 0u,
    };
    impl.postProcess.DdgiTrace().DispatchHeapIndexedThreads<Shaders::Modules::DdgiTraceCS>(
        impl.ctx, cmd, block, totalProbes, raysPerProbe, 1, push
    );
}

}
