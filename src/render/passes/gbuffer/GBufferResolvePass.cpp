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

    Vk::BufferBarrier::IndirectToTransfer(indirectBuf).Record(cmd);
    Vk::FillBuffer(cmd, indirectBuf);
    Vk::BufferBarrier::TransferToCompute(indirectBuf).Record(cmd);

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

    Vk::BufferBarrier::ComputeToIndirect(indirectBuf).Record(cmd);

    Vk::DynamicPass(in.sceneColor.Extent())
        .Viewport(sceneVp)
        .LoadColor(in.sceneColor)
        .LoadColor(in.velocity)
        .LoadColor(in.normRough)
        .LoadColor(in.bakedMask)
        .LoadDepth(in.depth)
        .Execute(cmd, [&]() {
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
        });
}

void RecordCpuCulled(
    RenderFrame&                 ctx,
    Vk::CommandEncoder&          encoder,
    const CsgResources&          csg,
    const MeshParticlePipelines& particles,
    const GBufferTargets&        in
) noexcept {
    auto* const cmd = encoder.cmd;

    Vk::DynamicPass(in.sceneColor.Extent())
        .Viewport(ctx.viewport)
        .LoadColor(in.sceneColor)
        .LoadColor(in.velocity)
        .LoadColor(in.normRough)
        .LoadColor(in.bakedMask)
        .LoadDepth(in.depth)
        .Execute(cmd, [&]() {
            DrawCSGMeshes(ctx, encoder, csg, in.sceneColor.Extent());
            Draw3DParticles(ctx, encoder, particles);
        });
}

} // namespace

void GBufferResolvePass::operator()(VkCommandBuffer cmd) const noexcept {
    Vk::CommandEncoder encoder(cmd);

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
