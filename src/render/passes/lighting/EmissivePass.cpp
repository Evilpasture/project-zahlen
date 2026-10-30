// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/EmissivePass.hpp"
#include "Renderer.hpp"
#include "passes/SceneDraw.hpp"
#include "passes/forward/ForwardResources.hpp"
#include <ShaderBindings.hpp>
#include <cstddef>
#include <cstdint>

namespace ZHLN::Passes {

void EmissivePass::operator()(VkCommandBuffer cmd) const noexcept {
    if (!forward.emissive.Valid()) {
        return;
    }

    auto& ctx = frame;

    Vk::CommandEncoder encoder(cmd);
    const auto         sceneVp        = frame.viewport;
    const auto& lightingTarget = frame.graphResources.lightingGI;
    const auto  lighting       = Vk::ImageSlice {
        lightingTarget.image.Handle(), lightingTarget.mipAttachmentViews[0], lightingTarget.extent, Res_LightingGI::format
    }.Assume<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>();
    const auto  depth = Vk::Assume<Vk::DepthStencilWrite<Res_Depth>>(frame.ActiveDepthTarget());

    Vk::DynamicPass(lighting.Extent()).Viewport(sceneVp).LoadColor(lighting).LoadDepth(depth).Execute(cmd, [&]() {
        for (size_t i = 0; i < ctx.renderer.queues.Draws().size(); ++i) {
            const auto& drawCmd = ctx.renderer.queues.Draws()[i];
            if (!HasDeferredEmission(drawCmd.instanceData.flags) || drawCmd.material == nullptr) {
                continue;
            }

            SubmitDrawInstanced(
                encoder, drawCmd, static_cast<uint32_t>(i), ObjectConstants {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0}, ctx.meshShading,
                forward.emissive.Get()
            );
        }
    });
}

} // namespace ZHLN::Passes
