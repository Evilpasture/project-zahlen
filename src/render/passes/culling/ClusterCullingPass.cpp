// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/culling/ClusterCullingPass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void ClusterCullingPass::operator()(VkCommandBuffer cmd) const noexcept {
    const uint32_t      fIdx          = frame.presenter.frameIndex;
    const Vk::Buffer&   counterBuffer = frame.frames.globalCounterBuffers[fIdx];

    Vk::FillBuffer(cmd, counterBuffer, 0, 0u);

    Vk::BufferBarrier(
        cmd, counterBuffer, Vk::BarrierStage::Transfer, Vk::BarrierAccess::TransferWrite, Vk::BarrierStage::Compute,
        Vk::BarrierAccess::ShaderRead | Vk::BarrierAccess::ShaderWrite
    );

    const Vk::HeapBlockBase block = frame.heapManager.WriteHeapParameters<Shaders::ClusterCulling>(
        cull.clusterCullingHeapBindings,
        Vk::Slot<"in_Bounds">(frame.clusterBoundsBuffer),
        Vk::Slot<"out_Grid">(frame.frames.clusterGridBuffers[fIdx]),
        Vk::Slot<"out_IndexList">(frame.frames.lightIndexListBuffers[fIdx]),
        Vk::Slot<"out_Counter">(frame.frames.globalCounterBuffers[fIdx]),
        Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"lights">(frame.frames.lightStorageBuffers[fIdx])
    );

    cull.clusterCullingPass.DispatchHeapIndexed(cmd, block);
}

}
