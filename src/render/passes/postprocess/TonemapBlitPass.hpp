// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PassPayloads.hpp"
#include "TargetManager.hpp"
#include "graph/RenderPassDecl.hpp"
#include "passes/FullscreenPassRecorder.hpp"
#include "passes/postprocess/PostProcessResources.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Core/FunctionRef.hpp>
#include <Zahlen/GraphicsSettings.hpp>

namespace ZHLN::Passes {

template <AAMode Mode>
using BlitInputRes = std::conditional_t<Mode != AAMode::None, Res_AccumCurrent, Res_HdrSceneColor>;

[[nodiscard]] constexpr auto NeedsManualSrgbOetf(const VkFormat format) noexcept -> bool {
    switch (format) {
        case VK_FORMAT_UNDEFINED:
        case VK_FORMAT_R8_SRGB:
        case VK_FORMAT_R8G8_SRGB:
        case VK_FORMAT_R8G8B8_SRGB:
        case VK_FORMAT_B8G8R8_SRGB:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return false;
        default:
            return true;
    }
}

template <AAMode Mode>
struct TonemapBlitPass:
    Vk::RenderPass<"Blit", Vk::ShaderRead<BlitInputRes<Mode>>, Vk::ShaderRead<Res_BloomFinal>, Vk::ShaderRead<Res_Depth>, Vk::ColorWrite<Res_Swapchain>> {
    using SwapchainImageFn = FunctionRef<Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>() const>;

    PostProcessResources& post;
    RenderFrameContext&   frame;
    SwapchainImageFn      getSwapchainImage;

    // Manual-scope pass (operator()(VkCommandBuffer)): the draw closure below
    // owns its Vk::DynamicPass scope -- the swapchain target is not a graph
    // binding, so the graph must not wrap this one in AutoCtx.
    void operator()(VkCommandBuffer cmd) const noexcept {
        using namespace Shaders;
        const auto swapchainTarget = getSwapchainImage();

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
                    .applySrgbOetf     = NeedsManualSrgbOetf(swapchainTarget.GetFormat()) ? 1 : 0
                };

                return MakeFullscreenPassArgs(
                    pc, Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<BlitInputRes<Mode>>>(blitInputImage)),
                    Vk::Slot<"texBloom">(Vk::Assume<Vk::ShaderRead<Res_BloomFinal>>(frame.graphResources.bloomFinalTarget)),
                    Vk::Unread<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_Depth>>(frame.ActiveDepthTarget())),
                    Vk::Unread<"frame">(frame.frames.frameUniformBuffers[fIdx])
                );
            },
            [&swapchainTarget](RenderFrameContext&, VkCommandBuffer drawCmd, auto&& execute) {
                Vk::DynamicPass(swapchainTarget.Extent()).OverwriteColor(swapchainTarget).Execute(drawCmd, [&]() { execute(); });
            }
        );
    }
};

} // namespace ZHLN::Passes
