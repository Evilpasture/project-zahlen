// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <utility>

namespace ZHLN::Passes {

// Which target feeds the tonemapper: the anti-aliased accumulation buffer when
// an AA pass ran, the raw HDR scene color when none did. It is a property of
// the AA mode the graph was compiled for, so it is resolved here at compile
// time rather than carried as a runtime flag.
template <AAMode Mode>
using BlitInputRes = std::conditional_t<Mode != AAMode::None, Res_AccumCurrent, Res_HdrSceneColor>;

// The whole display transform, in one payload. It has to mirror blit.slang
// field for field, which the two static_asserts below hold it to.
struct BlitPushConstants {
    float vignetteIntensity;
    float vignettePower;
    int   fullBright;
    float exposure;
    float bloomStrength;
    float contrast;
    float saturation;
    int   tonemapper;
    float colorFilter[3];
    float _padding;
};
static_assert(sizeof(BlitPushConstants) == 48, "BlitPushConstants must exactly mirror blit.slang");
static_assert(
    offsetof(BlitPushConstants, colorFilter) == 32 && offsetof(BlitPushConstants, _padding) == 44,
    "BlitPushConstants field offsets must exactly mirror blit.slang"
);

// Final display pass: exposure, bloom, tonemap, vignette and color grading in
// one fullscreen draw onto the presentation image.
//
// It is templated on the AA mode -- which resource it reads depends on it --
// and on the callable that vends this frame's swapchain image, since the
// destination is not a graph resource the binder owns but whatever the
// destination registry resolved this frame. That callable is a lambda declared
// where the graph is composed, so its type exists in no other translation
// unit: the body has to be here, at the point of instantiation, rather than in
// a .cpp of its own.
template <AAMode Mode, typename GetSwapchainImageT>
struct TonemapBlitPass: Vk::RenderPass<
                            "Blit", Vk::ShaderRead<BlitInputRes<Mode>>, Vk::ShaderRead<Res_BloomFinal>, Vk::ShaderRead<Res_Depth>,
                            Vk::ColorWrite<Res_Swapchain>> {
    RenderContext::Impl& impl;
    GetSwapchainImageT   getSwapchainImage;

    void operator()(VkCommandBuffer cmd) const noexcept {
        auto& blitInputImage = [&]() -> auto& {
            if constexpr (Mode != AAMode::None) {
                return impl.accumulationHistory.Current();
            } else {
                return impl.graphResources.hdrSceneColor;
            }
        }();

        const uint32_t fIdx = impl.presenter.frameIndex;

        const Vk::HeapBlockBase block = impl.blitPass.WriteHeapParameters<Shaders::Blit>(
            impl.ctx, impl.heapManager,
            Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<BlitInputRes<Mode>>>(blitInputImage)),
            Vk::Slot<"texBloom">(Vk::Assume<Vk::ShaderRead<Res_BloomFinal>>(impl.graphResources.bloomFinalTarget)),
            Vk::Unread<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_Depth>>(impl.presenter.depthTarget)),
            Vk::Unread<"frame">(impl.frames.frameUniformBuffers[fIdx])
        );

        const BlitPushConstants pc {
            .vignetteIntensity = impl.settings.post.vignetteIntensity,
            .vignettePower     = impl.settings.post.vignettePower,
            .fullBright        = impl.currentUniforms.fullBright != 0 ? 1 : 0,
            .exposure          = impl.settings.post.exposure,
            .bloomStrength     = impl.settings.post.bloomStrength,
            .contrast          = impl.settings.post.contrast,
            .saturation        = impl.settings.post.saturation,
            .tonemapper        = impl.settings.post.tonemapper,
            .colorFilter       = {impl.settings.post.colorFilter[0], impl.settings.post.colorFilter[1], impl.settings.post.colorFilter[2]},
            ._padding          = 0.0f
        };

        if (impl.blitPass.pipeline.Valid()) {
            const auto swapchainTarget = getSwapchainImage();

            impl.BindHeapsAndPushFrame(cmd);
            Vk::DynamicPass(swapchainTarget.extent).AddColor(swapchainTarget, VK_ATTACHMENT_LOAD_OP_DONT_CARE).Execute(cmd, [&]() {
                impl.blitPass.ExecuteHeap<Shaders::Modules::BlitPS>(impl.ctx, cmd, pc, block);
            });
        }
    }
};

}
