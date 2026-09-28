// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/gbuffer/GBufferResolvePass.hpp"
#include "passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>
#include <algorithm>

namespace ZHLN::Passes {

namespace {

// Second GPU-culling dispatch: `passIndex = 1` makes the shader read the
// candidate list the base pass's dispatch produced instead of testing every
// instance again, and the draw reads the second indirect buffer.
void RecordGpuCulled(PassContext& passCtx, const ZHLN::Array<GroupRange>& groups, uint32_t drawCount, const GBufferTargets& in) noexcept {
    VkCommandBuffer cmd = passCtx.Cmd();
    auto&           ctx = passCtx.ctx;
    const uint32_t  frameIndex = ctx.presenter.frameIndex;

    Vk::BufferBarrier(
        cmd, ctx.frames.indirectCommandsBuffersPass2[frameIndex].Handle(), Vk::BarrierStage::Compute | Vk::BarrierStage::Indirect,
        Vk::BarrierAccess::ShaderWrite | Vk::BarrierAccess::IndirectRead, Vk::BarrierStage::Clear, Vk::BarrierAccess::TransferWrite
    );

    Vk::FillBuffer(cmd, ctx.frames.indirectCommandsBuffersPass2[frameIndex], 0, 0u);

    Vk::BufferBarrier(
        cmd, ctx.frames.indirectCommandsBuffersPass2[frameIndex].Handle(), Vk::BarrierStage::Transfer, Vk::BarrierAccess::TransferWrite,
        Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite | Vk::BarrierAccess::ShaderRead
    );

    const uint32_t hizMips = std::min(ctx.graphResources.hizMap.mipLevels, kMaxGeneratedHiZMips);
    const auto     sceneVp = ctx.EffectiveViewport();
    const RenderContext::Impl::CullingConstants pc {
        .viewProj       = ctx.unjittered_view_proj,
        .hizScreenSize  = {sceneVp.width, sceneVp.height},
        .maxHiZMipLevel = hizMips > 0 ? hizMips - 1 : 0,
        .drawCount      = drawCount,
        .passIndex      = 1,
    };
    const Vk::HeapBlockBase block = ctx.heapManager.WriteHeapParameters<Shaders::Culling>(
        ctx.ctx, ctx.cullingHeapBindings, Vk::Slot<"g_instances">(ctx.frames.instanceDataBuffers[frameIndex]),
        Vk::Slot<"g_indirectCommands">(ctx.frames.indirectCommandsBuffersPass2[frameIndex]),
        Vk::Slot<"g_hizTexture">(Vk::Assume<Vk::ComputeRead<Res_HiZ>>(ctx.graphResources.hizMap)),
        Vk::Slot<"g_secondPassCandidates">(ctx.frames.secondPassCandidatesBuffers[frameIndex]),
        Vk::Slot<"g_secondPassCount">(ctx.frames.secondPassCountBuffers[frameIndex])
    );
    ctx.cullingPass.DispatchHeapIndexedThreads<Shaders::Modules::CullingCS>(ctx.ctx, cmd, block, drawCount, 1, 1, pc);

    using enum Vk::BarrierStage;
    using enum Vk::BarrierAccess;
    Vk::MemoryBarrier(cmd, Compute, ShaderWrite, Indirect, IndirectRead);

    Vk::DynamicPass(in.sceneColor.extent)
        .Viewport(sceneVp)
        .AddColor(in.sceneColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.velocity, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.normRough, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.emissive, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.clearcoat, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddDepth(in.depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .Execute(cmd, [&]() {
            passCtx.EnsureHeapState();

            for (const auto& group: groups) {
                if (!group.material->pipeline.Valid()) {
                    continue;
                }
                passCtx.encoder.DrawIndirect<Shaders::Modules::BasicVS, Shaders::Modules::BasicVSForward>(
                    {
                        .pipeline       = group.material->pipeline.Get(),
                        .layout         = group.material->layout,
                        .heap           = true,
                        .argumentBuffer = ctx.frames.indirectCommandsBuffersPass2[frameIndex].Handle(),
                        .offset         = Vk::DrawIndirectState::OffsetForIndex(group.start),
                        .drawCount      = group.count,
                    },
                    RenderContext::Impl::ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 0}
                );
            }
            DrawCSGMeshes(passCtx, in.sceneColor.extent);
            Draw3DParticles(passCtx);
        });
}

// CPU path: no second culling dispatch to replay, so this is only the work
// that could not run until the GBuffer's depth buffer was complete.
void RecordCpuCulled(PassContext& passCtx, const GBufferTargets& in) noexcept {
    VkCommandBuffer cmd = passCtx.Cmd();
    auto&           ctx = passCtx.ctx;

    Vk::DynamicPass(in.sceneColor.extent)
        .Viewport(ctx.EffectiveViewport())
        .AddColor(in.sceneColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.velocity, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.normRough, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.emissive, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.clearcoat, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddDepth(in.depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .Execute(cmd, [&]() {
            ctx.BindHeapsAndPushFrame(cmd);
            DrawCSGMeshes(passCtx, in.sceneColor.extent);
            Draw3DParticles(passCtx);
        });
}

} // namespace

void GBufferResolvePass::operator()(VkCommandBuffer cmd) const noexcept {
    PassContext passCtx(cmd, impl);

    const auto drawCount = static_cast<uint32_t>(impl.queues.Draws().size());
    if (drawCount == 0 && impl.queues.MeshParticleEmitters().empty() && impl.queues.CsgDraws().empty()) {
        return;
    }

    const ZHLN::Array<GroupRange> groups = BuildGroupRanges(impl);

    const bool useGpuCulling  = impl.cullingPass.pipeline.Valid() && impl.frames.indirectCommandsBuffers[impl.presenter.frameIndex].Valid() && (drawCount <= kGpuCullingMaxInstances) &&
                               !Diag::DisableGpuCulling() && !impl.MeshShadingActive();

    const GBufferTargets in = GBufferSceneTargets(impl);
    if (useGpuCulling) {
        RecordGpuCulled(passCtx, groups, drawCount, in);
    } else {
        RecordCpuCulled(passCtx, in);
    }
}

}
