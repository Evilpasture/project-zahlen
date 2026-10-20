// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PassPayloads.hpp"
#include "TargetManager.hpp"
#include "graph/RenderGraph.hpp"
#include "passes/FullscreenPassRecorder.hpp"
#include "passes/postprocess/PostProcessResources.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/GraphicsSettings.hpp>

namespace ZHLN::Passes {

template <AAMode Mode>
using BlitInputRes = std::conditional_t<Mode != AAMode::None, Res_AccumCurrent, Res_HdrSceneColor>;

template <AAMode Mode, typename GetSwapchainImageT>
struct TonemapBlitPass:
    Vk::RenderPass<"Blit", Vk::ShaderRead<BlitInputRes<Mode>>, Vk::ShaderRead<Res_BloomFinal>, Vk::ShaderRead<Res_Depth>, Vk::ColorWrite<Res_Swapchain>> {
    PostProcessResources& post;
    RenderFrameContext&   frame;
    GetSwapchainImageT    getSwapchainImage;

    // Manual-scope pass (operator()(VkCommandBuffer)): the draw closure below
    // owns its Vk::DynamicPass scope -- the swapchain target is not a graph
    // binding, so the graph must not wrap this one in AutoCtx.
    void operator()(VkCommandBuffer cmd) const noexcept {
        using namespace Shaders;
        FullscreenPassRecorder<Blit, Modules::BlitPS>::RecordCustom(
            frame, cmd, post.blit,
            [&]() {
                auto& blitInputImage = [&]() -> auto& {
                    if constexpr (Mode != AAMode::None) {
                        return frame.accumulationHistory.Current();
                    } else {
                        return frame.graphResources.hdrSceneColor;
                    }
                }();

                const auto              fIdx = frame.presenter.frameIndex;
                const BlitPushConstants pc {
                    .vignetteIntensity = frame.settings.post.vignetteIntensity,
                    .vignettePower     = frame.settings.post.vignettePower,
                    .fullBright        = frame.currentUniforms.fullBright != 0 ? 1 : 0,
                    .exposure          = frame.settings.post.exposure,
                    .bloomStrength     = frame.settings.post.bloomStrength,
                    .contrast          = frame.settings.post.contrast,
                    .saturation        = frame.settings.post.saturation,
                    .tonemapper        = frame.settings.post.tonemapper,
                    .colorFilter       = {frame.settings.post.colorFilter[0], frame.settings.post.colorFilter[1], frame.settings.post.colorFilter[2]},
                    ._padding          = 0.0f
                };

                return MakeFullscreenPassArgs(
                    pc, Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<BlitInputRes<Mode>>>(blitInputImage)),
                    Vk::Slot<"texBloom">(Vk::Assume<Vk::ShaderRead<Res_BloomFinal>>(frame.graphResources.bloomFinalTarget)),
                    Vk::Unread<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_Depth>>(frame.presenter.depthTarget)),
                    Vk::Unread<"frame">(frame.frames.frameUniformBuffers[fIdx])
                );
            },
            [this](RenderFrameContext& drawImpl, VkCommandBuffer drawCmd, auto&& execute) {
                const auto swapchainTarget = getSwapchainImage();

                drawImpl.BindHeapsAndPushFrame(drawCmd);
                Vk::DynamicPass(swapchainTarget.Extent()).OverwriteColor(swapchainTarget).Execute(drawCmd, [&]() { execute(); });
            }
        );
    }
};

} // namespace ZHLN::Passes
