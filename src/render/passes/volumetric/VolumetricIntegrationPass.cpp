// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/volumetric/VolumetricIntegrationPass.hpp"
#include "features/VolumetricFogSystem.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void VolumetricIntegrationPass::operator()(VkCommandBuffer cmd) const noexcept {
    const uint32_t fIdx = impl.presenter.frameIndex;

    const Vk::HeapBlockBase block = impl.fog.Integrate().WriteHeapParameters<Shaders::VolumetricIntegration>(
        impl.ctx, impl.heapManager,
        Vk::Slot<"inVoxelLight">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelLight>>(impl.graphResources.voxelLight)),
        Vk::Slot<"outVoxelIntegrated">(Vk::Assume<Vk::ComputeWrite<Res_VoxelInt>>(impl.graphResources.voxelIntegrated)),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx])
    );
    impl.fog.Integrate().DispatchHeap(impl.ctx, cmd, block);
}

}
