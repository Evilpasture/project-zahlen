// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/culling/ClusterCullingPass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void ClusterCullingPass::operator()(VkCommandBuffer cmd) const noexcept {
    const uint32_t      fIdx          = impl.presenter.frameIndex;
    const Vk::Buffer&   counterBuffer = impl.frames.globalCounterBuffers[fIdx];

    Vk::FillBuffer(cmd, counterBuffer, 0, 0u);

    Vk::BufferBarrier(
        cmd, counterBuffer, Vk::BarrierStage::Transfer, Vk::BarrierAccess::TransferWrite, Vk::BarrierStage::Compute,
        Vk::BarrierAccess::ShaderRead | Vk::BarrierAccess::ShaderWrite
    );

    const Vk::HeapBlockBase block = impl.heapManager.WriteHeapParameters<Shaders::ClusterCulling>(
        impl.ctx, impl.clusterCullingHeapBindings,
        Vk::Slot<"in_Bounds">(impl.clusterBoundsBuffer),
        Vk::Slot<"out_Grid">(impl.frames.clusterGridBuffers[fIdx]),
        Vk::Slot<"out_IndexList">(impl.frames.lightIndexListBuffers[fIdx]),
        Vk::Slot<"out_Counter">(impl.frames.globalCounterBuffers[fIdx]),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"lights">(impl.frames.lightStorageBuffers[fIdx])
    );

    impl.clusterCullingPass.DispatchHeapIndexed(impl.ctx, cmd, block);
}

}
