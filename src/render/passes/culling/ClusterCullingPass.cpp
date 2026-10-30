// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/culling/ClusterCullingPass.hpp"
#include "RenderFrame.hpp"
#include "Renderer.hpp"
#include "passes/culling/CullingResources.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void ClusterCullingPass::operator()(VkCommandBuffer cmd) const noexcept {
    const auto& counterBuffer = frame.globalCounterBuffers;

    Vk::FillBuffer(cmd, counterBuffer);
    Vk::BufferBarrier::TransferToCompute(counterBuffer).Record(cmd);

    const auto block = frame.renderer.heapManager.WriteHeapParameters<Shaders::ClusterCulling>(
        cull.clusterCullingHeapBindings, Vk::Slot<"in_Bounds">(frame.renderer.clusterBoundsBuffer), Vk::Slot<"out_Grid">(frame.clusterGridBuffers),
        Vk::Slot<"out_IndexList">(frame.lightIndexListBuffers), Vk::Slot<"out_Counter">(frame.globalCounterBuffers),
        Vk::Slot<"frame">(frame.frameUniformBuffers), Vk::Slot<"lights">(frame.lightStorageBuffers)
    );

    cull.clusterCullingPass.DispatchHeapIndexed(cmd, block);
}

} // namespace ZHLN::Passes
