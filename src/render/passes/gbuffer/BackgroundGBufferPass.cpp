// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/gbuffer/BackgroundGBufferPass.hpp"
#include "passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// Manual scope: the pass wraps its own Vk::DynamicPass. GBufferBasePass ran
// first and cleared the shared targets; this pass loads them and stamps the
// background layer. The depth test does the composition: the layer's clip
// depth is banded into [kSkyDepthSplit, 1] (BandedClipDepth), so it passes
// only where the world left no depth -- the map wins every pixel it covers.
void BackgroundGBufferPass::operator()(VkCommandBuffer cmd) const noexcept {
    frame.BindHeapsAndPushFrame(cmd);

    PassContext passCtx(cmd, frame);
    const auto  sceneVp = frame.EffectiveViewport();
    const auto  in      = GBufferSceneTargets(frame);

    // Vk::DynamicPass(in.sceneColor.Extent())
    //     .Viewport(sceneVp)
    //     .AddColor(in.sceneColor, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorScene)
    //     .AddColor(in.velocity, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorVelocity)
    //     .AddColor(in.normRough, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorNormalRoughness)
    //     .AddColor(in.emissive, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorEmissive)
    //     .AddColor(in.clearcoat, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorClearcoat)
    //     .AddColor(in.anisotropy, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorAnisotropy)
    //     .AddColor(in.sheen, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorSheen)
    //     .AddDepth(in.depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, kClearDepthValue)
    Vk::DynamicPass(in.sceneColor.Extent())
        .Viewport(sceneVp)
        .AddColor(in.sceneColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, kClearColorScene)
        .AddColor(in.velocity, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, kClearColorVelocity)
        .AddColor(in.normRough, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, kClearColorNormalRoughness)
        .AddColor(in.emissive, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, kClearColorEmissive)
        .AddColor(in.clearcoat, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, kClearColorClearcoat)
        .AddColor(in.anisotropy, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, kClearColorAnisotropy)
        .AddColor(in.sheen, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, kClearColorSheen)
        .AddDepth(in.depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, kClearDepthValue)
        .Execute(cmd, [&]() {
            for (size_t i = 0; i < frame.queues.Draws().size(); ++i) {
                const auto& drawCmd = frame.queues.Draws()[i];
                if ((drawCmd.flags & DrawFlags::Background) == DrawFlags::None || !IsVisibleIn(drawCmd.flags, RenderPassType::Main) ||
                    IsForwardOnly(drawCmd.instanceData.flags) || drawCmd.material == nullptr || drawCmd.material->pipeline == VK_NULL_HANDLE) {
                    continue;
                }

                const ObjectConstants push = {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0};

                // No pipeline override: this pass presents the same G-buffer
                // contract as GBufferBasePass, so it must resolve the pipeline
                // the same way. Passing drawCmd.material->pipeline here would
                // pin the background layer to the classic vertex path and drop
                // it off the meshlet path on GPUs that use it, for no gain --
                // SubmitDrawInstanced already falls back to that pipeline.
                SubmitDrawInstanced(passCtx.encoder, drawCmd, static_cast<uint32_t>(i), push, frame.MeshShadingActive());
            }
        });
}

} // namespace ZHLN::Passes
