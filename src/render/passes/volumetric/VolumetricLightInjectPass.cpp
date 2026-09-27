// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/volumetric/VolumetricLightInjectPass.hpp"
#include "features/VolumetricFogSystem.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// In-scattering strength, ambient floor, the Henyey-Greenstein anisotropy of
// the phase function, and whether the cascade map is consulted at all.
struct alignas(16) VolumetricLightInjectPushConstants {
    float    scatteringIntensity;
    float    ambientIntensity;
    float    phaseAnisotropy;
    uint32_t enableShadows;
};
static_assert(sizeof(VolumetricLightInjectPushConstants) == 16);
static_assert(GpuAbi::ScenePassPayload<VolumetricLightInjectPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void VolumetricLightInjectPass::operator()(VkCommandBuffer cmd) const noexcept {
    const uint32_t fIdx = impl.presenter.frameIndex;

    const Vk::HeapBlockBase block = impl.fog.LightInject().WriteHeapParameters<Shaders::VolumetricLightInject>(
        impl.ctx, impl.heapManager,
        Vk::Slot<"inVoxelMedia">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelMedia>>(impl.graphResources.voxelMedia)),
        Vk::Slot<"outVoxelLight">(Vk::Assume<Vk::ComputeWrite<Res_VoxelLight>>(impl.graphResources.voxelLight)),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"lights">(impl.frames.lightStorageBuffers[fIdx]),
        Vk::Slot<"clusterGrid">(impl.frames.clusterGridBuffers[fIdx]),
        Vk::Slot<"clusterIndexList">(impl.frames.lightIndexListBuffers[fIdx]),
        Vk::Slot<"shadowMap">(Vk::Assume<Vk::ComputeRead<Res_ShadowMap>>(impl.graphResources.shadowMap))
    );
    const VolumetricLightInjectPushConstants lightInjectPC = {};
    impl.fog.LightInject().DispatchHeap<Shaders::Modules::VolumetricLightInjectCS>(impl.ctx, cmd, block, lightInjectPC);
}

}
