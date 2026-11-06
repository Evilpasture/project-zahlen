// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/passes/gbuffer/GBufferCullingPass.hpp"

#include "render/RenderConstants.hpp"
#include "render/Renderer.hpp"
#include "render/passes/culling/CullingResources.hpp"
#include <ShaderBindings.hpp>
#include <algorithm>
#include <Zahlen/Log.hpp>

namespace ZHLN::Passes {

bool UsesGpuCulling(const CullingResources& cull, RenderFrame& frame, const bool secondPass, const uint32_t drawCount) noexcept {
    const auto& indirect = secondPass ? frame.indirectCommandsBuffersPass2 : frame.indirectCommandsBuffers;
    return cull.cullingPass.pipeline.Valid() && indirect.Valid() && drawCount != 0 && drawCount <= kGpuCullingMaxInstances && !Diag::DisableGpuCulling() &&
           !frame.meshShading;
}

void GBufferCullingPass::operator()(VkCommandBuffer cmd) const noexcept {
    const auto drawCount = static_cast<uint32_t>(frame.renderer.queues.Draws().size());
    if (!UsesGpuCulling(cull, frame, false, drawCount)) {
        return;
    }

    ZHLN::Log("[CullTmp] GPU culling pass 1 recorded: drawCount={}", drawCount);
    auto& countBuf = frame.secondPassCountBuffers;
    countBuf.SyncComputeForTransferWrite(cmd);
    Vk::FillBuffer(cmd, countBuf);
    countBuf.SyncForComputeReadWrite(cmd);

    const auto     sceneVp = frame.viewport;
    const uint32_t hizMips = std::min(frame.graphResources.hizMap.mipLevels, kMaxGeneratedHiZMips);

    const CullingConstants pc {
        .viewProj       = frame.unjitteredViewProj,
        .hizScreenSize  = {sceneVp.width, sceneVp.height},
        .maxHiZMipLevel = hizMips > 0 ? hizMips - 1 : 0,
        .drawCount      = drawCount,
        .passIndex      = 0,
    };

    const auto block = frame.renderer.heapManager.WriteHeapParameters<Shaders::Culling>(
        cull.cullingHeapBindings, Vk::Slot<"g_instances">(frame.instanceDataBuffers), Vk::Slot<"g_indirectCommands">(frame.indirectCommandsBuffers),
        Vk::Slot<"g_hizTexture">(Vk::Assume<Vk::ComputeRead<Res_HiZ>>(frame.graphResources.hizMap)),
        Vk::Slot<"g_secondPassCandidates">(frame.secondPassCandidatesBuffers), Vk::Slot<"g_secondPassCount">(countBuf)
    );
    cull.cullingPass.DispatchHeapIndexedThreads<Shaders::Modules::CullingCS>(cmd, block, drawCount, 1, 1, pc);

    frame.indirectCommandsBuffers.SyncForIndirectRead(cmd);
    frame.secondPassCandidatesBuffers.SyncForComputeRead(cmd);
    countBuf.SyncForComputeRead(cmd);
}

void GBufferResolveCullingPass::operator()(VkCommandBuffer cmd) const noexcept {
    const auto drawCount = static_cast<uint32_t>(frame.renderer.queues.Draws().size());
    if (!UsesGpuCulling(cull, frame, true, drawCount)) {
        return;
    }

    auto&       indirectBuf = frame.indirectCommandsBuffersPass2;
    const auto  sceneVp     = frame.viewport;
    const auto  hizMips     = std::min(frame.graphResources.hizMap.mipLevels, kMaxGeneratedHiZMips);
    const CullingConstants pc {
        .viewProj       = frame.unjitteredViewProj,
        .hizScreenSize  = {sceneVp.width, sceneVp.height},
        .maxHiZMipLevel = hizMips > 0 ? hizMips - 1 : 0,
        .drawCount      = drawCount,
        .passIndex      = 1,
    };

    indirectBuf.SyncIndirectForTransferWrite(cmd);
    Vk::FillBuffer(cmd, indirectBuf);
    indirectBuf.SyncForComputeReadWrite(cmd);

    const auto block = frame.renderer.heapManager.WriteHeapParameters<Shaders::Culling>(
        cull.cullingHeapBindings, Vk::Slot<"g_instances">(frame.instanceDataBuffers), Vk::Slot<"g_indirectCommands">(indirectBuf),
        Vk::Slot<"g_hizTexture">(Vk::Assume<Vk::ComputeRead<Res_HiZ>>(frame.graphResources.hizMap)),
        Vk::Slot<"g_secondPassCandidates">(frame.secondPassCandidatesBuffers), Vk::Slot<"g_secondPassCount">(frame.secondPassCountBuffers)
    );
    cull.cullingPass.DispatchHeapIndexedThreads<Shaders::Modules::CullingCS>(cmd, block, drawCount, 1, 1, pc);

    indirectBuf.SyncForIndirectRead(cmd);
}

} // namespace ZHLN::Passes
