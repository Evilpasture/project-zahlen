// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/gbuffer/GBufferBasePass.hpp"
#include "passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <tuple>

namespace ZHLN::Passes {

namespace {

static_assert(
    [] {
        constexpr auto colorA =
            Vk::ImageSlice {VK_NULL_HANDLE, VK_NULL_HANDLE, VkExtent2D {}, VK_FORMAT_R8G8B8A8_UNORM}.Assume<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>();
        constexpr auto colorB =
            Vk::ImageSlice {VK_NULL_HANDLE, VK_NULL_HANDLE, VkExtent2D {}, VK_FORMAT_B8G8R8A8_SRGB}.Assume<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>();
        constexpr auto depthStencil = Vk::ImageSlice {VK_NULL_HANDLE, VK_NULL_HANDLE, VkExtent2D {}, VK_FORMAT_D32_SFLOAT_S8_UINT}
                                          .Assume<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL>();
        constexpr auto depthOnly =
            Vk::ImageSlice {VK_NULL_HANDLE, VK_NULL_HANDLE, VkExtent2D {}, VK_FORMAT_D32_SFLOAT}.Assume<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>();

        constexpr auto pass = Vk::DynamicPass(VkExtent2D {.width = 64, .height = 32})
                                  .Viewport(2.0F, 3.0F, 30.0F, 20.0F)
                                  .AddColor(colorA)
                                  .AddColorGroup(std::tuple {colorB})
                                  .AddDepth(depthStencil)
                                  .Flags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT)
                                  .ViewMask(3);
        static constexpr auto inherit   = pass.GetSecondaryInheritance();
        constexpr auto        colors    = inherit.ColorFormats();
        constexpr auto        noStencil = Vk::DynamicPass(VkExtent2D {.width = 64, .height = 32})
                                       .AddDepth(depthOnly)
                                       .Flags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT)
                                       .GetSecondaryInheritance();

        return colors.size() == 2 && colors[0] == colorA.GetFormat() && colors[1] == colorB.GetFormat() && inherit.depthFormat == depthStencil.GetFormat() &&
               inherit.stencilFormat == depthStencil.GetFormat() && inherit.viewMask == 3 && inherit.viewport.x == 2.0F && inherit.viewport.width == 30.0F &&
               noStencil.ColorFormats().empty() && noStencil.depthFormat == depthOnly.GetFormat() && noStencil.stencilFormat == VK_FORMAT_UNDEFINED;
    }(),
    "Secondary inheritance must match the DynamicPass's bound runtime attachments."
);

struct TaskSystemSchedulerAdapter {
    void ParallelFor(uint32_t count, uint32_t chunkSize, auto&& func) const {
        TaskSystem::ParallelFor(count, chunkSize, std::forward<decltype(func)>(func));
    }
};

void RecordGpuCulled(
    RenderFrameContext&         ctx,
    Vk::CommandEncoder&         encoder,
    CullingResources&           cull,
    std::span<const GroupRange> groups,
    uint32_t                    drawCount,
    const GBufferTargets&       in
) noexcept {
    using enum Vk::BarrierStage;
    using enum Vk::BarrierAccess;
    auto* const cmd        = encoder.cmd;
    const auto  frameIndex = ctx.presenter.frameIndex;

    Vk::BufferBarrier(cmd, ctx.frames.secondPassCountBuffers[frameIndex].Handle(), Compute | Indirect, ShaderWrite | IndirectRead, Clear, TransferWrite);

    Vk::FillBuffer(cmd, ctx.frames.secondPassCountBuffers[frameIndex]);

    Vk::BufferBarrier(cmd, ctx.frames.secondPassCountBuffers[frameIndex].Handle(), Transfer, TransferWrite, Compute, ShaderWrite | ShaderRead);

    const auto     sceneVp = ctx.EffectiveViewport();
    const uint32_t hizMips = std::min(ctx.graphResources.hizMap.mipLevels, kMaxGeneratedHiZMips);

    const CullingConstants pc {
        .viewProj       = ctx.unjittered_view_proj,
        .hizScreenSize  = {sceneVp.width, sceneVp.height},
        .maxHiZMipLevel = hizMips > 0 ? hizMips - 1 : 0,
        .drawCount      = drawCount,
        .passIndex      = 0,
    };

    const auto block = ctx.heapManager.WriteHeapParameters<Shaders::Culling>(
        cull.cullingHeapBindings, Vk::Slot<"g_instances">(ctx.frames.instanceDataBuffers[frameIndex]),
        Vk::Slot<"g_indirectCommands">(ctx.frames.indirectCommandsBuffers[frameIndex]),
        Vk::Slot<"g_hizTexture">(Vk::Assume<Vk::ComputeRead<Res_HiZ>>(ctx.graphResources.hizMap)),
        Vk::Slot<"g_secondPassCandidates">(ctx.frames.secondPassCandidatesBuffers[frameIndex]),
        Vk::Slot<"g_secondPassCount">(ctx.frames.secondPassCountBuffers[frameIndex])
    );
    cull.cullingPass.DispatchHeapIndexedThreads<Shaders::Modules::CullingCS>(cmd, block, drawCount, 1, 1, pc);

    Vk::MemoryBarrier(cmd, Compute, ShaderWrite, Indirect, IndirectRead);

    Vk::DynamicPass(in.sceneColor.Extent())
        .Viewport(sceneVp)
        .ClearColor(in.sceneColor, kClearColorScene)
        .ClearColor(in.velocity, kClearColorVelocity)
        .ClearColor(in.normRough, kClearColorNormalRoughness)
        .ClearColor(in.bakedMask, kClearColorBakedMask)
        .ClearDepth(in.depth, kClearDepthValue)
        .Execute(cmd, [&]() {
            for (const auto& group: groups) {
                if (group.material->pipeline == VK_NULL_HANDLE) {
                    continue;
                }
                encoder.SetCullMode(group.cullMode);
                encoder.DrawIndirect<Shaders::Modules::BasicVS, Shaders::Modules::BasicVSForward>(
                    {
                        .pipeline       = group.material->pipeline,
                        .argumentBuffer = ctx.frames.indirectCommandsBuffers[frameIndex].Handle(),
                        .offset         = Vk::DrawIndirectState::OffsetForIndex(group.start),
                        .drawCount      = group.count,
                    },
                    ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 0}
                );
            }
        });
}

void RecordCpuCulled(RenderFrameContext& ctx, VkCommandBuffer cmd, uint32_t drawCount, const GBufferTargets& in) noexcept {
    using enum DrawFlags;
    const uint32_t frameIndex = ctx.presenter.frameIndex;
    const auto     sceneVp    = ctx.EffectiveViewport();

    const auto pass = Vk::DynamicPass(in.sceneColor.Extent())
                          .Viewport(sceneVp)
                          .ClearColor(in.sceneColor, kClearColorScene)
                          .ClearColor(in.velocity, kClearColorVelocity)
                          .ClearColor(in.normRough, kClearColorNormalRoughness)
                          .ClearColor(in.bakedMask, kClearColorBakedMask)
                          .ClearDepth(in.depth, kClearDepthValue)
                          .Flags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
    pass.Execute(cmd, [&]() {
        const auto frameAddresses = ctx.FrameHeapAddresses();
        const auto samplerBind    = ctx.heapManager.GetSamplerHeapBindInfo();
        const auto resourceBind   = ctx.heapManager.GetResourceHeapBindInfo();

        const auto chunks = Vk::ParallelDrawDispatch(
            cmd,
            pass.GetSecondaryInheritance(
                &samplerBind, &resourceBind, GpuAbi::kScenePushLayout.UsedFrameAddresses(),
                std::span<const VkDeviceAddress> {frameAddresses.data(), frameAddresses.size()}
            ),
            drawCount, kParallelChunkSize, TaskSystemSchedulerAdapter {},
            [&](uint32_t) -> VkCommandBuffer {
                auto wIdx = TaskSystem::GetWorkerIndex();
                if (wIdx >= ctx.workerCmds.size()) {
                    wIdx = static_cast<uint32_t>(ctx.workerCmds.size() - 1);
                }
                const auto localCmdIdx = ctx.workerCmds[wIdx].cmdCount[frameIndex].fetch_add(1, std::memory_order::relaxed);
                return ctx.workerCmds[wIdx].pools[frameIndex][localCmdIdx];
            },
            [&](Vk::CommandEncoder& encoder, uint32_t i) {
                const auto& drawCmd = ctx.queues.Draws()[i];
                // Background scenery draws only in BackgroundGBufferPass; a
                // second stamp here would write the skybox into the world
                // pass's cleared G-buffer ahead of its own pass. (Both passes
                // now share one depth range -- see kSkyDepthSplit -- so this is
                // about ordering, not about bands.)
                if (!IsVisibleIn(drawCmd.flags, RenderPassType::Main) || (drawCmd.flags & DrawFlags::Viewmodel) != DrawFlags::None ||
                    (drawCmd.flags & DrawFlags::Background) != DrawFlags::None || drawCmd.material->pipeline == VK_NULL_HANDLE ||
                    IsForwardOnly(drawCmd.instanceData.flags)) {
                    return;
                }
                SubmitDrawInstanced(encoder, drawCmd, i, ObjectConstants {.instanceId = i, .isShadowPass = 0}, ctx.MeshShadingActive());
            }
        );

        // The batch never executes with an error in it -- a pass split across
        // secondaries cannot run half of its draws -- and what it adds is which
        // chunk failed and why. A chunk is one worker's slice of the draw range,
        // so this also says how much of the pass was lost.
        chunks.ForEachError([](const Vk::RecordingFailure& failure) {
            ZHLN::Log("[Render] Parallel G-buffer chunk {} could not be recorded ({}); the batch was not submitted.", failure.index, failure.error);
        });
    });
}

} // namespace

void GBufferBasePass::operator()(VkCommandBuffer cmd) const noexcept {
    Vk::CommandEncoder encoder(cmd);

    const auto drawCount = static_cast<uint32_t>(frame.queues.Draws().size());
    if (drawCount == 0) {
        const auto in = GBufferSceneTargets(frame);
        Vk::DynamicPass(in.sceneColor.Extent())
            .ClearColor(in.sceneColor, kClearColorScene)
            .ClearColor(in.velocity, kClearColorVelocity)
            .ClearColor(in.normRough, kClearColorNormalRoughness)
            .ClearColor(in.bakedMask, kClearColorBakedMask)
            .ClearDepth(in.depth, kClearDepthValue)
            .Execute(cmd, []() {});
        return;
    }

    const auto groups = BuildGroupRanges(frame);
    const auto in     = GBufferSceneTargets(frame);
    if (cull.cullingPass.pipeline.Valid() && frame.frames.indirectCommandsBuffers[frame.presenter.frameIndex].Valid() &&
        (drawCount <= kGpuCullingMaxInstances) && !Diag::DisableGpuCulling() && !frame.MeshShadingActive()) {
        RecordGpuCulled(frame, encoder, cull, groups, drawCount, in);
    } else {
        RecordCpuCulled(frame, cmd, drawCount, in);
    }
}

} // namespace ZHLN::Passes
