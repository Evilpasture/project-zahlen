// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/volumetric/VolumetricIntegrationPass.hpp"
#include "features/VolumetricFogSystem.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void VolumetricIntegrationPass::operator()(VkCommandBuffer cmd) const noexcept {
    const auto fIdx = frame.presenter.frameIndex;

    const auto block = frame.fog.Integrate().WriteHeapParameters<Shaders::VolumetricIntegration>(
        frame.heapManager, Vk::Slot<"inVoxelLight">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelLight>>(frame.graphResources.voxelLight)),
        Vk::Slot<"outVoxelIntegrated">(Vk::Assume<Vk::ComputeWrite<Res_VoxelInt>>(frame.graphResources.voxelIntegrated)),
        Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx])
    );
    frame.fog.Integrate().DispatchHeap(cmd, block);
}

} // namespace ZHLN::Passes
