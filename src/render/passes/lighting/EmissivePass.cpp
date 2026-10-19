// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/EmissivePass.hpp"
#include "passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>
#include <cstddef>
#include <cstdint>

namespace ZHLN::Passes {

void EmissivePass::operator()(VkCommandBuffer cmd) const noexcept {
    if (!forward.emissive.Valid()) {
        return;
    }

    auto& ctx = frame;
    ctx.BindHeapsAndPushFrame(cmd);

    PassContext passCtx(cmd, frame);
    const auto  sceneVp = frame.EffectiveViewport();
    const auto  lighting = Vk::Assume<Vk::ColorWrite<Res_LightingGI>>(frame.graphResources.lightingGI);
    const auto  depth    = Vk::Assume<Vk::DepthStencilWrite<Res_Depth>>(frame.presenter.depthTarget);

    Vk::DynamicPass(lighting.Extent())
        .Viewport(sceneVp)
        .AddColor(lighting, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddDepth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .Execute(cmd, [&]() {
            for (size_t i = 0; i < ctx.queues.Draws().size(); ++i) {
                const auto& drawCmd = ctx.queues.Draws()[i];
                if (!HasDeferredEmission(drawCmd.instanceData.flags) || drawCmd.material == nullptr) {
                    continue;
                }

                SubmitDrawInstanced(
                    passCtx.encoder, drawCmd, static_cast<uint32_t>(i), ObjectConstants {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0},
                    ctx.MeshShadingActive(), forward.emissive.Get()
                );
            }
        });
}

} // namespace ZHLN::Passes
