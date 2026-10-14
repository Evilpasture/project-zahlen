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
    const uint32_t fIdx   = impl.presenter.frameIndex;
    const bool     wantRT = impl.settings.rayTracing.enableDiffuseGI ||
                            (impl.settings.rayTracing.enableReflections && impl.settings.post.enableRTR != 0) ||
                            impl.settings.rayTracing.enableShadows;
    if (!impl.ctx.RayTracingSupported() || !wantRT || impl.settings.environment.fullBright != 0 || impl.frames.tlasAddresses[fIdx] == nullptr) {
        return;
    }

    impl.BindHeapsAndPushFrame(cmd);

    const Vk::HeapBlockBase block = impl.heapManager.WriteHeapParameters<Shaders::DdgiUpdateIrradiance>(
        impl.postProcess.DdgiUpdateHeapBindings(),
        Vk::Slot<"inRays">(Vk::Assume<Vk::ComputeRead<Res_DdgiRays>>(impl.graphResources.ddgiRays)),
        Vk::Slot<"outIrradiance">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.ddgiIrradiance)),
        Vk::Slot<"outDistance">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(impl.graphResources.ddgiDistance)),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx])
    );

    const uint32_t totalProbes  = impl.graphResources.ddgiRays.extent.width;
    const uint32_t raysPerProbe = impl.graphResources.ddgiRays.extent.height;

    const DdgiUpdatePushConstants push {
        .totalProbes    = totalProbes,
        .raysPerProbe   = raysPerProbe,
        .hysteresis     = impl.currentUniforms.ddgiParams.x,
        .depthSharpness = 16.0f,
    };
    impl.postProcess.DdgiUpdate().DispatchHeapIndexedThreads<Shaders::Modules::DdgiUpdateIrradianceCS>(
        cmd, block, totalProbes * 16u, 16u, 1u, push
    );
}

}
