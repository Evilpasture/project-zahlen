// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/gbuffer/GBufferResolvePass.hpp"
#include "Renderer.hpp"
#include "passes/SceneDraw.hpp"
#include "passes/culling/CullingResources.hpp"
#include <ShaderBindings.hpp>
#include <algorithm>

namespace ZHLN::Passes {

namespace {

void RecordGpuCulled(
    RenderFrame&                 ctx,
    Vk::CommandEncoder&          encoder,
    CullingResources&            cull,
    const CsgResources&          csg,
    const MeshParticlePipelines& particles,
    std::span<const GroupRange>  groups,
    uint32_t                     drawCount,
    const GBufferTargets&        in
) noexcept {
    auto* const    cmd         = encoder.cmd;
    const auto&    indirectBuf = ctx.indirectCommandsBuffersPass2;

    indirectBuf.SyncIndirectForTransferWrite(cmd);
    Vk::FillBuffer(cmd, indirectBuf);
    indirectBuf.SyncForComputeReadWrite(cmd);

    const auto             hizMips = std::min(ctx.graphResources.hizMap.mipLevels, kMaxGeneratedHiZMips);
    const auto             sceneVp = ctx.viewport;
    const CullingConstants pc {
        .viewProj       = ctx.unjitteredViewProj,
        .hizScreenSize  = {sceneVp.width, sceneVp.height},
        .maxHiZMipLevel = hizMips > 0 ? hizMips - 1 : 0,
        .drawCount      = drawCount,
        .passIndex      = 1,
    };
    const auto block = ctx.renderer.heapManager.WriteHeapParameters<Shaders::Culling>(
        cull.cullingHeapBindings, Vk::Slot<"g_instances">(ctx.instanceDataBuffers), Vk::Slot<"g_indirectCommands">(indirectBuf),
        Vk::Slot<"g_hizTexture">(Vk::Assume<Vk::ComputeRead<Res_HiZ>>(ctx.graphResources.hizMap)),
        Vk::Slot<"g_secondPassCandidates">(ctx.secondPassCandidatesBuffers), Vk::Slot<"g_secondPassCount">(ctx.secondPassCountBuffers)
    );
    cull.cullingPass.DispatchHeapIndexedThreads<Shaders::Modules::CullingCS>(cmd, block, drawCount, 1, 1, pc);

    indirectBuf.SyncForIndirectRead(cmd);

    encoder.SetViewport(sceneVp);

    for (const auto& group: groups) {
        if (group.material->pipeline == VK_NULL_HANDLE) {
            continue;
        }
        encoder.DrawIndirect<Shaders::Modules::BasicVS, Shaders::Modules::BasicVSForward>(
            {
                .pipeline       = group.material->pipeline,
                .argumentBuffer = indirectBuf.Handle(),
                .offset         = Vk::DrawIndirectState::OffsetForIndex(group.start),
                .drawCount      = group.count,
            },
            ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 0}
        );
    }

    DrawCSGMeshes(ctx, encoder, csg, in.sceneColor.Extent());
    Draw3DParticles(ctx, encoder, particles);
}

void RecordCpuCulled(
    RenderFrame&                 ctx,
    Vk::CommandEncoder&          encoder,
    const CsgResources&          csg,
    const MeshParticlePipelines& particles,
    const GBufferTargets&        in
) noexcept {
    auto* const cmd = encoder.cmd;

    encoder.SetViewport(ctx.viewport);

    // The graph holds the scope open on the same five targets; these two draw
    // calls are all this path adds inside it.
    DrawCSGMeshes(ctx, encoder, csg, in.sceneColor.Extent());
    Draw3DParticles(ctx, encoder, particles);
}

} // namespace

void GBufferResolvePass::operator()(Vk::RasterPassContextBase& pass) const noexcept {
    // Unlike the first G-buffer pass this one is not the frame's first writer:
    // GBufferBasePass already cleared and filled these targets, so the state
    // table hands this pass LOAD instead of CLEAR -- which is exactly what the
    // manual LoadColor/LoadDepth list stated.
    pass.StateViewport(frame.viewport);

    Vk::CommandEncoder encoder(pass.Cmd());

    const auto drawCount = static_cast<uint32_t>(frame.renderer.queues.Draws().size());
    if (drawCount == 0 && frame.renderer.queues.MeshParticleEmitters().empty() && frame.renderer.queues.CsgDraws().empty()) {
        return;
    }

    const auto groups = BuildGroupRanges(frame);
    const auto in     = GBufferSceneTargets(frame);
    if (cull.cullingPass.pipeline.Valid() && frame.indirectCommandsBuffers.Valid() && (drawCount <= kGpuCullingMaxInstances) && !Diag::DisableGpuCulling() &&
        !frame.meshShading) {
        RecordGpuCulled(frame, encoder, cull, csg, particles, groups, drawCount, in);
    } else {
        RecordCpuCulled(frame, encoder, csg, particles, in);
    }
}

} // namespace ZHLN::Passes
