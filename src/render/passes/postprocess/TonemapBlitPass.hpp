// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PassPayloads.hpp"
#include "Renderer.hpp"
#include "TargetManager.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>
#include "passes/FullscreenPassRecorder.hpp"
#include "passes/postprocess/PostProcessResources.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Core/FunctionRef.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>

namespace ZHLN::Passes {

template <AAMode Mode>
using BlitInputRes = std::conditional_t<Mode != AAMode::None, Res_AccumCurrent, Res_HdrSceneColor>;

[[nodiscard]] constexpr auto NeedsManualSrgbOetf(const Vk::Format format) noexcept -> bool {
    switch (format) {
        case Vk::Format::Undefined:
        case Vk::Format::R8Srgb:
        case Vk::Format::R8G8Srgb:
        case Vk::Format::R8G8B8Srgb:
        case Vk::Format::B8G8R8Srgb:
        case Vk::Format::R8G8B8A8Srgb:
        case Vk::Format::B8G8R8A8Srgb:
        case Vk::Format::A8B8G8R8SrgbPack32:
        case Vk::Format::R16G16B16A16Sfloat:
        case Vk::Format::R32G32B32A32Sfloat:
        case Vk::Format::B10G11R11UfloatPack32:
            return false;
        default:
            return true;
    }
}

template <AAMode Mode>
struct TonemapBlitPass:
    VkD3D::RenderPass<"Blit", VkD3D::ShaderRead<BlitInputRes<Mode>>, VkD3D::ShaderRead<Res_BloomFinal>, VkD3D::ShaderRead<Res_Depth>, VkD3D::ColorWrite<Res_Swapchain>> {
    using SwapchainImageFn = FunctionRef<Vk::TypedImage<Vk::ImageLayout::ColorAttachmentOptimal>() const>;

    PostProcessResources& post;
    RenderFrame&          frame;
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
                        return frame.renderer.accumulationHistory.Current();
                    } else {
                        return frame.graphResources.hdrSceneColor;
                    }
                }();

                const BlitPushConstants pc {
                    .vignetteIntensity = frame.settings.post.vignetteIntensity,
                    .vignettePower     = frame.settings.post.vignettePower,
                    .fullBright        = frame.uniforms.fullBright != 0 ? 1 : 0,
                    .exposure          = frame.settings.post.exposure,
                    .bloomStrength     = frame.settings.post.bloomStrength,
                    .contrast          = frame.settings.post.contrast,
                    .saturation        = frame.settings.post.saturation,
                    .tonemapper        = frame.settings.post.tonemapper,
                    .colorFilter       = {frame.settings.post.colorFilter[0], frame.settings.post.colorFilter[1], frame.settings.post.colorFilter[2]},
                    .applySrgbOetf     = NeedsManualSrgbOetf(swapchainTarget.GetFormat()) ? 1 : 0,
                    .rcasSharpness     = frame.settings.post.rcasSharpness
                };

                return MakeFullscreenPassArgs(
                    pc, Vk::Slot<"texInput">(Vk::Assume<VkD3D::ShaderRead<BlitInputRes<Mode>>>(blitInputImage)),
                    Vk::Slot<"texBloom">(Vk::Assume<VkD3D::ShaderRead<Res_BloomFinal>>(frame.graphResources.bloomFinalTarget)),
                    Vk::Unread<"texDepth">(Vk::Assume<VkD3D::ShaderRead<Res_Depth>>(frame.ActiveDepthTarget())), Vk::Unread<"frame">(frame.frameUniformBuffers)
                );
            },
            [&swapchainTarget](RenderFrame&, VkCommandBuffer drawCmd, auto&& execute) {
                Vk::DynamicPass(swapchainTarget.Extent()).OverwriteColor(swapchainTarget).Execute(drawCmd, [&]() { execute(); });
            }
        );
    }
};

} // namespace ZHLN::Passes
