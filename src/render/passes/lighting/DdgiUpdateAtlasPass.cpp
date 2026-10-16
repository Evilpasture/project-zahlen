// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/DdgiUpdateAtlasPass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

struct DdgiUpdatePushConstants {
    uint32_t totalProbes;
    uint32_t raysPerProbe;
    float    hysteresis;
    float    depthSharpness;
};
static_assert(GpuAbi::ScenePassPayload<DdgiUpdatePushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void DdgiUpdateAtlasPass::operator()(VkCommandBuffer cmd) const noexcept {
    const auto fIdx   = frame.presenter.frameIndex;
    const auto wantRT = frame.settings.rayTracing.enableDiffuseGI || (frame.settings.rayTracing.enableReflections && frame.settings.post.enableRTR != 0) ||
                        frame.settings.rayTracing.enableShadows;
    if (!frame.ctx.RayTracingSupported() || !wantRT || frame.settings.environment.fullBright != 0 || frame.frames.tlasAddresses[fIdx] == nullptr) {
        return;
    }

    frame.BindHeapsAndPushFrame(cmd);

    const auto block = frame.heapManager.WriteHeapParameters<Shaders::DdgiUpdateIrradiance>(
        frame.postProcess.DdgiUpdateHeapBindings(), Vk::Slot<"inRays">(Vk::Assume<Vk::ComputeRead<Res_DdgiRays>>(frame.graphResources.ddgiRays)),
        Vk::Slot<"outIrradiance">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.ddgiIrradiance)),
        Vk::Slot<"outDistance">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.ddgiDistance)),
        Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx])
    );

    const auto totalProbes  = frame.graphResources.ddgiRays.extent.width;
    const auto raysPerProbe = frame.graphResources.ddgiRays.extent.height;

    const DdgiUpdatePushConstants push {
        .totalProbes    = totalProbes,
        .raysPerProbe   = raysPerProbe,
        .hysteresis     = frame.currentUniforms.ddgiParams.x,
        .depthSharpness = 16.0f,
    };
    frame.postProcess.DdgiUpdate().DispatchHeapIndexedThreads<Shaders::Modules::DdgiUpdateIrradianceCS>(cmd, block, totalProbes * 16u, 16u, 1u, push);
}

} // namespace ZHLN::Passes
