// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/culling/ClusterCullingPass.hpp"
#include "FrameContext.hpp"
#include "passes/culling/CullingResources.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void ClusterCullingPass::operator()(VkCommandBuffer cmd) const noexcept {
    const auto  fIdx          = frame.presenter.frameIndex;
    const auto& counterBuffer = frame.frames.globalCounterBuffers[fIdx];

    Vk::FillBuffer(cmd, counterBuffer);
    Vk::BufferBarrier::TransferToCompute(counterBuffer).Record(cmd);

    const auto block = frame.heapManager.WriteHeapParameters<Shaders::ClusterCulling>(
        cull.clusterCullingHeapBindings, Vk::Slot<"in_Bounds">(frame.clusterBoundsBuffer), Vk::Slot<"out_Grid">(frame.frames.clusterGridBuffers[fIdx]),
        Vk::Slot<"out_IndexList">(frame.frames.lightIndexListBuffers[fIdx]), Vk::Slot<"out_Counter">(frame.frames.globalCounterBuffers[fIdx]),
        Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]), Vk::Slot<"lights">(frame.frames.lightStorageBuffers[fIdx])
    );

    cull.clusterCullingPass.DispatchHeapIndexed(cmd, block);
}

} // namespace ZHLN::Passes
