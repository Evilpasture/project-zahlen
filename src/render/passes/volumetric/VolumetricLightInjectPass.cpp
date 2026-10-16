// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/volumetric/VolumetricLightInjectPass.hpp"
#include "features/VolumetricFogSystem.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

struct alignas(16) VolumetricLightInjectPushConstants {
    float    scatteringIntensity;
    float    ambientIntensity;
    float    phaseAnisotropy;
    uint32_t enableShadows;
};
static_assert(sizeof(VolumetricLightInjectPushConstants) == 16);
static_assert(
    GpuAbi::ScenePassPayload<VolumetricLightInjectPushConstants>,
    "a pass payload that outgrew the push blob's prefix, asserted where it is declared"
);

void VolumetricLightInjectPass::operator()(VkCommandBuffer cmd) const noexcept {
    const auto fIdx = frame.presenter.frameIndex;

    const auto block = frame.fog.LightInject().WriteHeapParameters<Shaders::VolumetricLightInject>(
        frame.heapManager, Vk::Slot<"inVoxelMedia">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelMedia>>(frame.graphResources.voxelMedia)),
        Vk::Slot<"outVoxelLight">(Vk::Assume<Vk::ComputeWrite<Res_VoxelLight>>(frame.graphResources.voxelLight)),
        Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]), Vk::Slot<"lights">(frame.frames.lightStorageBuffers[fIdx]),
        Vk::Slot<"clusterGrid">(frame.frames.clusterGridBuffers[fIdx]), Vk::Slot<"clusterIndexList">(frame.frames.lightIndexListBuffers[fIdx]),
        Vk::Slot<"shadowMap">(Vk::Assume<Vk::ComputeRead<Res_ShadowMap>>(frame.graphResources.shadowMap))
    );
    const VolumetricLightInjectPushConstants lightInjectPC = {};
    frame.fog.LightInject().DispatchHeap<Shaders::Modules::VolumetricLightInjectCS>(cmd, block, lightInjectPC);
}

} // namespace ZHLN::Passes
