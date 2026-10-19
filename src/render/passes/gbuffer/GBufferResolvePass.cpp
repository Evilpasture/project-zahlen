// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/gbuffer/GBufferResolvePass.hpp"
#include "passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>
#include <algorithm>

namespace ZHLN::Passes {

namespace {

void RecordGpuCulled(
    PassContext&                 passCtx,
    CullingResources&            cull,
    const CsgResources&          csg,
    const MeshParticlePipelines& particles,
    std::span<const GroupRange>  groups,
    uint32_t                     drawCount,
    const GBufferTargets&        in
) noexcept {
    using enum Vk::BarrierStage;
    using enum Vk::BarrierAccess;
    auto* const    cmd        = passCtx.Cmd();
    auto&          ctx        = passCtx.ctx;
    const uint32_t frameIndex = ctx.presenter.frameIndex;

    Vk::BufferBarrier(cmd, ctx.frames.indirectCommandsBuffersPass2[frameIndex].Handle(), Compute | Indirect, ShaderWrite | IndirectRead, Clear, TransferWrite);
    Vk::FillBuffer(cmd, ctx.frames.indirectCommandsBuffersPass2[frameIndex]);
    Vk::BufferBarrier(cmd, ctx.frames.indirectCommandsBuffersPass2[frameIndex].Handle(), Transfer, TransferWrite, Compute, ShaderWrite | ShaderRead);

    const auto             hizMips = std::min(ctx.graphResources.hizMap.mipLevels, kMaxGeneratedHiZMips);
    const auto             sceneVp = ctx.EffectiveViewport();
    const CullingConstants pc {
        .viewProj       = ctx.unjittered_view_proj,
        .hizScreenSize  = {sceneVp.width, sceneVp.height},
        .maxHiZMipLevel = hizMips > 0 ? hizMips - 1 : 0,
        .drawCount      = drawCount,
        .passIndex      = 1,
    };
    const auto block = ctx.heapManager.WriteHeapParameters<Shaders::Culling>(
        cull.cullingHeapBindings, Vk::Slot<"g_instances">(ctx.frames.instanceDataBuffers[frameIndex]),
        Vk::Slot<"g_indirectCommands">(ctx.frames.indirectCommandsBuffersPass2[frameIndex]),
        Vk::Slot<"g_hizTexture">(Vk::Assume<Vk::ComputeRead<Res_HiZ>>(ctx.graphResources.hizMap)),
        Vk::Slot<"g_secondPassCandidates">(ctx.frames.secondPassCandidatesBuffers[frameIndex]),
        Vk::Slot<"g_secondPassCount">(ctx.frames.secondPassCountBuffers[frameIndex])
    );
    cull.cullingPass.DispatchHeapIndexedThreads<Shaders::Modules::CullingCS>(cmd, block, drawCount, 1, 1, pc);

    Vk::MemoryBarrier(cmd, Compute, ShaderWrite, Indirect, IndirectRead);

    Vk::DynamicPass(in.sceneColor.Extent())
        .Viewport(sceneVp)
        .AddColor(in.sceneColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.velocity, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.normRough, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddDepth(in.depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .Execute(cmd, [&]() {
            passCtx.EnsureHeapState();

            for (const auto& group: groups) {
                if (group.material->pipeline == VK_NULL_HANDLE) {
                    continue;
                }
                passCtx.encoder.DrawIndirect<Shaders::Modules::BasicVS, Shaders::Modules::BasicVSForward>(
                    {
                        .pipeline       = group.material->pipeline,
                        .argumentBuffer = ctx.frames.indirectCommandsBuffersPass2[frameIndex].Handle(),
                        .offset         = Vk::DrawIndirectState::OffsetForIndex(group.start),
                        .drawCount      = group.count,
                    },
                    ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 0}
                );
            }
            DrawCSGMeshes(passCtx, csg, in.sceneColor.Extent());
            Draw3DParticles(passCtx, particles);
        });
}

void RecordCpuCulled(PassContext& passCtx, const CsgResources& csg, const MeshParticlePipelines& particles, const GBufferTargets& in) noexcept {
    auto* const cmd = passCtx.Cmd();
    const auto& ctx = passCtx.ctx;

    Vk::DynamicPass(in.sceneColor.Extent())
        .Viewport(ctx.EffectiveViewport())
        .AddColor(in.sceneColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.velocity, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.normRough, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddDepth(in.depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .Execute(cmd, [&]() {
            ctx.BindHeapsAndPushFrame(cmd);
            DrawCSGMeshes(passCtx, csg, in.sceneColor.Extent());
            Draw3DParticles(passCtx, particles);
        });
}

} // namespace

void GBufferResolvePass::operator()(VkCommandBuffer cmd) const noexcept {
    PassContext passCtx(cmd, frame);

    const auto drawCount = static_cast<uint32_t>(frame.queues.Draws().size());
    if (drawCount == 0 && frame.queues.MeshParticleEmitters().empty() && frame.queues.CsgDraws().empty()) {
        return;
    }

    const auto groups = BuildGroupRanges(frame);
    const auto in     = GBufferSceneTargets(frame);
    if (cull.cullingPass.pipeline.Valid() && frame.frames.indirectCommandsBuffers[frame.presenter.frameIndex].Valid() &&
        (drawCount <= kGpuCullingMaxInstances) && !Diag::DisableGpuCulling() && !frame.MeshShadingActive()) {
        RecordGpuCulled(passCtx, cull, csg, particles, groups, drawCount, in);
    } else {
        RecordCpuCulled(passCtx, csg, particles, in);
    }
}

} // namespace ZHLN::Passes
