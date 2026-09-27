// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/volumetric/VolumetricFogInjectPass.hpp"
#include "features/VolumetricFogSystem.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// One fog volume's media parameters. The four float3 colors plus their
// trailing scalar keep this at 80 bytes, which is what the shader declares.
struct alignas(16) VolumetricFogPushConstants {
    float density;
    float heightFalloff;
    float heightOffset;
    float anisotropy;

    float scatteringColor[3];
    float noiseScale;

    float absorptionColor[3];
    float noiseSpeed;

    float emissiveColor[3];
    float noiseIntensity;

    uint32_t volumeCount;
    uint32_t enableNoise;
    uint32_t _pad0;
    uint32_t _pad1;
};
static_assert(sizeof(VolumetricFogPushConstants) == 80);
static_assert(GpuAbi::ScenePassPayload<VolumetricFogPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void VolumetricFogInjectPass::operator()(VkCommandBuffer cmd) const noexcept {
    const uint32_t fIdx = impl.presenter.frameIndex;

    const Vk::HeapBlockBase block = impl.fog.FogInject().WriteHeapParameters<Shaders::VolumetricFogInject>(
        impl.ctx, impl.heapManager,
        Vk::Slot<"outVoxelMedia">(Vk::Assume<Vk::ComputeWrite<Res_VoxelMedia>>(impl.graphResources.voxelMedia)),
        Vk::Slot<"noiseTexture">(impl.fog.NoiseWrite()),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"fogVolumes">(impl.frames.fogVolumesBuffer[fIdx])
    );

    const VolumetricFogPushConstants fogPC = {};
    impl.fog.FogInject().DispatchHeap<Shaders::Modules::VolumetricFogInjectCS>(impl.ctx, cmd, block, fogPC);
}

}
