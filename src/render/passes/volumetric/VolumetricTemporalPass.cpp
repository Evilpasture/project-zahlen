// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/volumetric/VolumetricTemporalPass.hpp"
#include "features/VolumetricFogSystem.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// How much history survives, how hard the neighbourhood clamp bites, and a
// reset flag for the first frame after a resize or a target recreation.
struct alignas(16) VolumetricTemporalPushConstants {
    float    temporalWeight;
    float    clampStrength;
    uint32_t resetHistory;
    uint32_t _pad;
};
static_assert(sizeof(VolumetricTemporalPushConstants) == 16);
static_assert(GpuAbi::ScenePassPayload<VolumetricTemporalPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void VolumetricTemporalPass::operator()(VkCommandBuffer cmd) const noexcept {
    const uint32_t fIdx = impl.presenter.frameIndex;

    const Vk::HeapBlockBase block = impl.fog.Temporal().WriteHeapParameters<Shaders::VolumetricTemporal>(
        impl.ctx, impl.heapManager,
        Vk::Slot<"inVoxelIntegratedCurrent">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelInt>>(impl.graphResources.voxelIntegrated)),
        Vk::Slot<"inVoxelIntegratedHistory">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelHist>>(impl.graphResources.voxelHistory)),
        Vk::Slot<"outVoxelIntegratedResolved">(Vk::Assume<Vk::ComputeWrite<Res_VoxelResolved>>(impl.graphResources.voxelResolved)),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx])
    );
    const VolumetricTemporalPushConstants temporalPC = {};

    impl.fog.Temporal().DispatchHeap<Shaders::Modules::VolumetricTemporalCS>(impl.ctx, cmd, block, temporalPC);
}

}
