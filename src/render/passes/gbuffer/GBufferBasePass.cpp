// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/gbuffer/GBufferBasePass.hpp"
#include "Renderer.hpp"
#include "passes/SceneDraw.hpp"
#include "passes/culling/CullingResources.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <array>
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
        // Asserted where the driver reads it: the inheritance chain's own struct,
        // not the fields that happen to produce it.
        static constexpr auto inherit = pass.GetInheritance();
        VkCommandBufferInheritanceRenderingInfo info {};
        [[maybe_unused]] const void* head = inherit.Link(nullptr, info);

        constexpr auto noStencilPass = Vk::DynamicPass(VkExtent2D {.width = 64, .height = 32})
                                           .AddDepth(depthOnly)
                                           .Flags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
        static constexpr auto noStencil = noStencilPass.GetInheritance();
        VkCommandBufferInheritanceRenderingInfo noStencilInfo {};
        [[maybe_unused]] const void* noStencilHead = noStencil.Link(nullptr, noStencilInfo);

        return info.colorAttachmentCount == 2 && info.pColorAttachmentFormats != nullptr && info.pColorAttachmentFormats[0] == colorA.GetFormat() &&
               info.pColorAttachmentFormats[1] == colorB.GetFormat() && info.depthAttachmentFormat == depthStencil.GetFormat() &&
               info.stencilAttachmentFormat == depthStencil.GetFormat() && info.viewMask == 3 && info.rasterizationSamples == VK_SAMPLE_COUNT_1_BIT &&
               noStencilInfo.colorAttachmentCount == 0 && noStencilInfo.pColorAttachmentFormats == nullptr &&
               noStencilInfo.depthAttachmentFormat == depthOnly.GetFormat() && noStencilInfo.stencilAttachmentFormat == VK_FORMAT_UNDEFINED;
    }(),
    "Secondary inheritance must match the DynamicPass's bound runtime attachments."
);

struct TaskSystemSchedulerAdapter {
    void ParallelFor(uint32_t count, uint32_t chunkSize, auto&& func) const {
        TaskSystem::ParallelFor(count, chunkSize, std::forward<decltype(func)>(func));
    }
};

void RecordGpuCulled(
    RenderFrame&                ctx,
    Vk::CommandEncoder&         encoder,
    CullingResources&           cull,
    std::span<const GroupRange> groups,
    uint32_t                    drawCount,
    const GBufferTargets&       in
) noexcept {
    auto* const cmd      = encoder.cmd;
    const auto& countBuf = ctx.secondPassCountBuffers;

    Vk::BufferBarrier::ComputeToTransfer(countBuf).Record(cmd);
    Vk::FillBuffer(cmd, countBuf);
    Vk::BufferBarrier::TransferToCompute(countBuf).Record(cmd);

    const auto     sceneVp = ctx.viewport;
    const uint32_t hizMips = std::min(ctx.graphResources.hizMap.mipLevels, kMaxGeneratedHiZMips);

    const CullingConstants pc {
        .viewProj       = ctx.unjitteredViewProj,
        .hizScreenSize  = {sceneVp.width, sceneVp.height},
        .maxHiZMipLevel = hizMips > 0 ? hizMips - 1 : 0,
        .drawCount      = drawCount,
        .passIndex      = 0,
    };

    const auto block = ctx.renderer.heapManager.WriteHeapParameters<Shaders::Culling>(
        cull.cullingHeapBindings, Vk::Slot<"g_instances">(ctx.instanceDataBuffers), Vk::Slot<"g_indirectCommands">(ctx.indirectCommandsBuffers),
        Vk::Slot<"g_hizTexture">(Vk::Assume<Vk::ComputeRead<Res_HiZ>>(ctx.graphResources.hizMap)),
        Vk::Slot<"g_secondPassCandidates">(ctx.secondPassCandidatesBuffers), Vk::Slot<"g_secondPassCount">(countBuf)
    );
    cull.cullingPass.DispatchHeapIndexedThreads<Shaders::Modules::CullingCS>(cmd, block, drawCount, 1, 1, pc);

    Vk::BufferBarrier::Record(
        cmd, std::to_array({
                 Vk::BufferBarrier::ComputeToIndirect(ctx.indirectCommandsBuffers),
                 Vk::BufferBarrier::ComputeWriteToRead(ctx.secondPassCandidatesBuffers),
                 Vk::BufferBarrier::ComputeWriteToRead(countBuf),
             })
    );

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
                        .argumentBuffer = ctx.indirectCommandsBuffers.Handle(),
                        .offset         = Vk::DrawIndirectState::OffsetForIndex(group.start),
                        .drawCount      = group.count,
                    },
                    ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 0}
                );
            }
        });
}

void RecordCpuCulled(RenderFrame& ctx, VkCommandBuffer cmd, uint32_t drawCount, const GBufferTargets& in) noexcept {
    using enum DrawFlags;
    const uint32_t frameIndex = ctx.inFlight;
    const auto     sceneVp    = ctx.viewport;

    const auto pass = Vk::DynamicPass(in.sceneColor.Extent())
                          .Viewport(sceneVp)
                          .ClearColor(in.sceneColor, kClearColorScene)
                          .ClearColor(in.velocity, kClearColorVelocity)
                          .ClearColor(in.normRough, kClearColorNormalRoughness)
                          .ClearColor(in.bakedMask, kClearColorBakedMask)
                          .ClearDepth(in.depth, kClearDepthValue)
                          .Flags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
    pass.Execute(cmd, [&]() {
        const auto secContext = ctx.renderer.CreateSecondaryContext(pass.GetInheritance());

        const auto chunks = Vk::ParallelDrawDispatch(
            cmd, secContext, drawCount, kParallelChunkSize, TaskSystemSchedulerAdapter {},
            [&](uint32_t) -> VkCommandBuffer {
                auto wIdx = TaskSystem::GetWorkerIndex();
                if (wIdx >= ctx.renderer.workerCmds.size()) {
                    wIdx = static_cast<uint32_t>(ctx.renderer.workerCmds.size() - 1);
                }
                const auto localCmdIdx = ctx.renderer.workerCmds[wIdx].cmdCount[frameIndex].fetch_add(1, std::memory_order::relaxed);
                return ctx.renderer.workerCmds[wIdx].pools[frameIndex][localCmdIdx];
            },
            [&](Vk::CommandEncoder& encoder, uint32_t i) {
                const auto& drawCmd = ctx.renderer.queues.Draws()[i];
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
                SubmitDrawInstanced(encoder, drawCmd, i, ObjectConstants {.instanceId = i, .isShadowPass = 0}, ctx.meshShading);
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

    const auto drawCount = static_cast<uint32_t>(frame.renderer.queues.Draws().size());
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
    if (cull.cullingPass.pipeline.Valid() && frame.indirectCommandsBuffers.Valid() && (drawCount <= kGpuCullingMaxInstances) && !Diag::DisableGpuCulling() &&
        !frame.meshShading) {
        RecordGpuCulled(frame, encoder, cull, groups, drawCount, in);
    } else {
        RecordCpuCulled(frame, cmd, drawCount, in);
    }
}

} // namespace ZHLN::Passes
