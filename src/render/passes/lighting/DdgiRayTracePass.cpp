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
    const auto fIdx = frame.presenter.frameIndex;
    if (!frame.ctx.RayTracingSupported() ||
        !(frame.settings.rayTracing.enableDiffuseGI || (frame.settings.rayTracing.enableReflections && frame.settings.post.enableRTR != 0) ||
          frame.settings.rayTracing.enableShadows) ||
        frame.settings.environment.fullBright != 0 || frame.frames.tlasAddresses[fIdx] == nullptr) {
        return;
    }

    frame.BindHeapsAndPushFrame(cmd);

    const Vk::AsAddressWrite tlas {.address = frame.frames.tlasAddresses[fIdx].Get()};
    const auto               block = frame.heapManager.WriteHeapParameters<Shaders::DdgiTrace>(
        frame.postProcess.DdgiTraceHeapBindings(), Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"g_instances">(frame.frames.instanceDataBuffers[fIdx]),
        Vk::Slot<"texDdgiIrradiance">(Vk::Assume<Vk::ComputeRead<Res_DdgiIrradiance>>(frame.graphResources.ddgiIrradiance)),
        Vk::Slot<"texDdgiDistance">(Vk::Assume<Vk::ComputeRead<Res_DdgiDistance>>(frame.graphResources.ddgiDistance)),
        Vk::Slot<"outRays">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.ddgiRays)), Vk::Slot<"tlas">(tlas)
    );

    const auto totalProbes  = frame.graphResources.ddgiRays.extent.width;
    const auto raysPerProbe = frame.graphResources.ddgiRays.extent.height;

    const DdgiTracePushConstants push {
        .totalProbes    = totalProbes,
        .raysPerProbe   = raysPerProbe,
        .maxRayDistance = 24.0f,
        ._pad           = 0u,
    };
    frame.postProcess.DdgiTrace().DispatchHeapIndexedThreads<Shaders::Modules::DdgiTraceCS>(cmd, block, totalProbes, raysPerProbe, 1, push);
}

} // namespace ZHLN::Passes
