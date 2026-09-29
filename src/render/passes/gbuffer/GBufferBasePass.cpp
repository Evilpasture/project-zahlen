// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/gbuffer/GBufferBasePass.hpp"
#include "passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <tuple>

namespace ZHLN::Passes {

namespace {

// GBufferSceneTargets uses runtime-format TypedImages (AssumeLayout), so a
// secondary must inherit the formats AddColor/AddDepth actually recorded.
// Exercise both a combined stencil attachment and a depth-only attachment.
static_assert([] {
    constexpr auto colorA = Vk::ImageSlice {VK_NULL_HANDLE, VK_NULL_HANDLE, VkExtent2D {}, VK_FORMAT_R8G8B8A8_UNORM}
                                .Assume<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>();
    constexpr auto colorB = Vk::ImageSlice {VK_NULL_HANDLE, VK_NULL_HANDLE, VkExtent2D {}, VK_FORMAT_B8G8R8A8_SRGB}
                                .Assume<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>();
    constexpr auto depthStencil = Vk::ImageSlice {VK_NULL_HANDLE, VK_NULL_HANDLE, VkExtent2D {}, VK_FORMAT_D32_SFLOAT_S8_UINT}
                                      .Assume<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL>();
    constexpr auto depthOnly = Vk::ImageSlice {VK_NULL_HANDLE, VK_NULL_HANDLE, VkExtent2D {}, VK_FORMAT_D32_SFLOAT}
                                   .Assume<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>();

    const auto pass = Vk::DynamicPass(VkExtent2D {.width = 64, .height = 32})
        .Viewport(2.0F, 3.0F, 30.0F, 20.0F)
        .AddColor(colorA)
        .AddColorGroup(std::tuple {colorB})
        .AddDepth(depthStencil)
        .Flags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT)
        .ViewMask(3);
    const auto inherit = pass.GetSecondaryInheritance();
    const auto colors = inherit.ColorFormats();
    const auto noStencil = Vk::DynamicPass(VkExtent2D {.width = 64, .height = 32})
        .AddDepth(depthOnly)
        .Flags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT)
        .GetSecondaryInheritance();

    return colors.size() == 2 && colors[0] == colorA.GetFormat() && colors[1] == colorB.GetFormat() &&
           inherit.depthFormat == depthStencil.GetFormat() && inherit.stencilFormat == depthStencil.GetFormat() && inherit.viewMask == 3 &&
           inherit.viewport.x == 2.0F && inherit.viewport.width == 30.0F &&
           noStencil.ColorFormats().empty() && noStencil.depthFormat == depthOnly.GetFormat() && noStencil.stencilFormat == VK_FORMAT_UNDEFINED;
}(), "Secondary inheritance must match the DynamicPass's bound runtime attachments.");

struct TaskSystemSchedulerAdapter {
    void ParallelFor(uint32_t count, uint32_t chunkSize, auto&& func) const {
        TaskSystem::ParallelFor(count, chunkSize, std::forward<decltype(func)>(func));
    }
};

// GPU culling: one dispatch fills the indirect command buffer, then the
// pipeline-sorted runs are drawn from it. The HiZ pyramid is the rejection
// test, so this is also the point where the second-pass candidate list is
// produced for the resolve pass to pick up.
void RecordGpuCulled(PassContext& passCtx, const ZHLN::Array<GroupRange>& groups, uint32_t drawCount, const GBufferTargets& in) noexcept {
    VkCommandBuffer cmd = passCtx.Cmd();
    auto&           ctx = passCtx.ctx;
    const uint32_t  frameIndex = ctx.presenter.frameIndex;

    Vk::BufferBarrier(
        cmd, ctx.frames.secondPassCountBuffers[frameIndex].Handle(), Vk::BarrierStage::Compute | Vk::BarrierStage::Indirect,
        Vk::BarrierAccess::ShaderWrite | Vk::BarrierAccess::IndirectRead, Vk::BarrierStage::Clear, Vk::BarrierAccess::TransferWrite
    );

    Vk::FillBuffer(cmd, ctx.frames.secondPassCountBuffers[frameIndex], 0, 0u);

    Vk::BufferBarrier(
        cmd, ctx.frames.secondPassCountBuffers[frameIndex].Handle(), Vk::BarrierStage::Transfer, Vk::BarrierAccess::TransferWrite,
        Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite | Vk::BarrierAccess::ShaderRead
    );

    const auto sceneVp     = ctx.EffectiveViewport();
    const uint32_t hizMips = std::min(ctx.graphResources.hizMap.mipLevels, kMaxGeneratedHiZMips);

    const RenderContext::Impl::CullingConstants pc {
        .viewProj       = ctx.unjittered_view_proj,
        .hizScreenSize  = {sceneVp.width, sceneVp.height},
        .maxHiZMipLevel = hizMips > 0 ? hizMips - 1 : 0,
        .drawCount      = drawCount,
        .passIndex      = 0,
    };

    const auto block = ctx.heapManager.WriteHeapParameters<Shaders::Culling>(
        ctx.ctx, ctx.cullingHeapBindings, Vk::Slot<"g_instances">(ctx.frames.instanceDataBuffers[frameIndex]),
        Vk::Slot<"g_indirectCommands">(ctx.frames.indirectCommandsBuffers[frameIndex]),
        Vk::Slot<"g_hizTexture">(Vk::Assume<Vk::ComputeRead<Res_HiZ>>(ctx.graphResources.hizMap)),
        Vk::Slot<"g_secondPassCandidates">(ctx.frames.secondPassCandidatesBuffers[frameIndex]),
        Vk::Slot<"g_secondPassCount">(ctx.frames.secondPassCountBuffers[frameIndex])
    );
    ctx.cullingPass.DispatchHeapIndexedThreads<Shaders::Modules::CullingCS>(ctx.ctx, cmd, block, drawCount, 1, 1, pc);

    using enum Vk::BarrierStage;
    using enum Vk::BarrierAccess;
    Vk::MemoryBarrier(cmd, Compute, ShaderWrite, Indirect, IndirectRead);

    Vk::DynamicPass(in.sceneColor.Extent())
        .Viewport(sceneVp)
        .AddColor(in.sceneColor, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorScene)
        .AddColor(in.velocity, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorVelocity)
        .AddColor(in.normRough, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorNormalRoughness)
        .AddColor(in.emissive, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorEmissive)
        .AddColor(in.clearcoat, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorClearcoat)
        .AddColor(in.anisotropy, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorAnisotropy)
        .AddDepth(in.depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearDepthValue)
        .Execute(cmd, [&]() {
            passCtx.EnsureHeapState();

            for (const auto& group: groups) {
                if (group.material->pipeline == VK_NULL_HANDLE) {
                    continue;
                }
                passCtx.encoder.DrawIndirect<Shaders::Modules::BasicVS, Shaders::Modules::BasicVSForward>(
                    {
                        .pipeline       = group.material->pipeline,
                        .layout         = group.material->layout,
                        .heap           = true,
                        .argumentBuffer = ctx.frames.indirectCommandsBuffers[frameIndex].Handle(),
                        .offset         = Vk::DrawIndirectState::OffsetForIndex(group.start),
                        .drawCount      = group.count,
                    },
                    RenderContext::Impl::ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 0}
                );
            }
        });
}

// CPU culling: no indirect buffer, so the draw queue is replayed in parallel
// across worker secondaries inside one render pass begun with the
// secondary-command-buffer flag.
void RecordCpuCulled(PassContext& passCtx, uint32_t drawCount, const GBufferTargets& in) noexcept {
    VkCommandBuffer cmd        = passCtx.Cmd();
    auto&           ctx        = passCtx.ctx;
    const uint32_t  frameIndex = ctx.presenter.frameIndex;
    const auto      sceneVp    = ctx.EffectiveViewport();

    const auto pass = Vk::DynamicPass(in.sceneColor.Extent())
        .Viewport(sceneVp)
        .AddColor(in.sceneColor, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorScene)
        .AddColor(in.velocity, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorVelocity)
        .AddColor(in.normRough, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorNormalRoughness)
        .AddColor(in.emissive, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorEmissive)
        .AddColor(in.clearcoat, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorClearcoat)
        .AddColor(in.anisotropy, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorAnisotropy)
        .AddDepth(in.depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearDepthValue)
        .Flags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
    pass.Execute(cmd, [&]() {
        ctx.BindHeapsAndPushFrame(cmd);
        const auto frameAddresses = ctx.FrameHeapAddresses();
        const auto samplerBind    = ctx.heapManager.GetSamplerHeapBindInfo();
        const auto resourceBind   = ctx.heapManager.GetResourceHeapBindInfo();

        Vk::ParallelDrawDispatch(
            cmd,
            pass.GetSecondaryInheritance(
                &samplerBind, &resourceBind, GpuAbi::kScenePushLayout.UsedFrameAddresses(),
                std::span<const VkDeviceAddress> {frameAddresses.data(), frameAddresses.size()}
            ),
            drawCount, kParallelChunkSize, TaskSystemSchedulerAdapter {},
            [&](uint32_t ) -> VkCommandBuffer {
                uint32_t wIdx = TaskSystem::GetWorkerIndex();
                if (wIdx >= ctx.workerCmds.size()) {
                    wIdx = static_cast<uint32_t>(ctx.workerCmds.size() - 1);
                }
                uint32_t localCmdIdx = ctx.workerCmds[wIdx].cmdCount[frameIndex].fetch_add(1, std::memory_order::relaxed);
                return ctx.workerCmds[wIdx].pools[frameIndex][localCmdIdx];
            },
            [&](Vk::CommandEncoder& encoder, uint32_t i) {
                const auto& drawCmd = ctx.queues.Draws()[i];
                if (!IsVisibleIn(drawCmd.flags, RenderPassType::Main) || (drawCmd.flags & DrawFlags::Viewmodel) != DrawFlags::None ||
                    drawCmd.material->pipeline == VK_NULL_HANDLE || IsForwardOnly(drawCmd.instanceData.flags)) {
                    return;
                }
                SubmitDrawInstanced(encoder, drawCmd, i, RenderContext::Impl::ObjectConstants {.instanceId = i, .isShadowPass = 0}, ctx.MeshShadingActive());
            }
        );
    });
}

} // namespace

void GBufferBasePass::operator()(VkCommandBuffer cmd) const noexcept {
    PassContext passCtx(cmd, impl);

    const auto drawCount = static_cast<uint32_t>(impl.queues.Draws().size());
    if (drawCount == 0) {
        // Nothing to draw, but the GBuffer still has to be in a defined state:
        // clear it rather than leave last frame's contents in the targets.
        const GBufferTargets in = GBufferSceneTargets(impl);
        Vk::DynamicPass(in.sceneColor.Extent())
            .AddColor(in.sceneColor, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorScene)
            .AddColor(in.velocity, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorVelocity)
            .AddColor(in.normRough, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorNormalRoughness)
            .AddColor(in.emissive, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorEmissive)
            .AddColor(in.clearcoat, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorClearcoat)
            .AddColor(in.anisotropy, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorAnisotropy)
            .AddDepth(in.depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearDepthValue)
            .Execute(cmd, []() {});
        return;
    }

    const ZHLN::Array<GroupRange> groups = BuildGroupRanges(impl);

    const bool useGpuCulling  = impl.cullingPass.pipeline.Valid() && impl.frames.indirectCommandsBuffers[impl.presenter.frameIndex].Valid() && (drawCount <= kGpuCullingMaxInstances) &&
                               !Diag::DisableGpuCulling() && !impl.MeshShadingActive();

    const GBufferTargets in = GBufferSceneTargets(impl);
    if (useGpuCulling) {
        RecordGpuCulled(passCtx, groups, drawCount, in);
    } else {
        RecordCpuCulled(passCtx, drawCount, in);
    }
}

}
