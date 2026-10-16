// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/volumetric/VolumetricTemporalPass.hpp"
#include "features/VolumetricFogSystem.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

struct alignas(16) VolumetricTemporalPushConstants {
    float    temporalWeight;
    float    clampStrength;
    uint32_t resetHistory;
    uint32_t _pad;
};
static_assert(sizeof(VolumetricTemporalPushConstants) == 16);
static_assert(GpuAbi::ScenePassPayload<VolumetricTemporalPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void VolumetricTemporalPass::operator()(VkCommandBuffer cmd) const noexcept {
    const auto fIdx = frame.presenter.frameIndex;

    const auto block = frame.fog.Temporal().WriteHeapParameters<Shaders::VolumetricTemporal>(
        frame.heapManager, Vk::Slot<"inVoxelIntegratedCurrent">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelInt>>(frame.graphResources.voxelIntegrated)),
        Vk::Slot<"inVoxelIntegratedHistory">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelHist>>(frame.graphResources.voxelHistory)),
        Vk::Slot<"outVoxelIntegratedResolved">(Vk::Assume<Vk::ComputeWrite<Res_VoxelResolved>>(frame.graphResources.voxelResolved)),
        Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx])
    );
    const VolumetricTemporalPushConstants temporalPC = {};

    frame.fog.Temporal().DispatchHeap<Shaders::Modules::VolumetricTemporalCS>(cmd, block, temporalPC);
}

} // namespace ZHLN::Passes
