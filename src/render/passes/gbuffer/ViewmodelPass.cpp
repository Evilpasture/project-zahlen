// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/gbuffer/ViewmodelPass.hpp"
#include "passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void ViewmodelPass::operator()(VkCommandBuffer cmd) const noexcept {
    bool hasViewmodelDraws = false;
    for (const auto& drawCmd: impl.queues.Draws()) {
        if ((drawCmd.flags & DrawFlags::Viewmodel) != DrawFlags::None && !IsForwardOnly(drawCmd.instanceData.flags)) {
            hasViewmodelDraws = true;
            break;
        }
    }

    if (!hasViewmodelDraws) {
        return;
    }

    impl.BindHeapsAndPushFrame(cmd);

    PassContext passCtx(cmd, impl);
    const GBufferTargets in = GBufferSceneTargets(impl);

    Vk::DynamicPass(in.sceneColor.Extent())
        .Viewport(impl.EffectiveViewport())
        .AddColor(in.sceneColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.velocity, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.normRough, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.emissive, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.clearcoat, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddDepth(in.depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .Execute(cmd, [&]() {
            for (size_t i = 0; i < impl.queues.Draws().size(); ++i) {
                const auto& drawCmd = impl.queues.Draws()[i];

                if ((drawCmd.flags & DrawFlags::Viewmodel) == DrawFlags::None || IsForwardOnly(drawCmd.instanceData.flags)) {
                    continue;
                }

                if (!drawCmd.material->pipeline.Valid()) {
                    continue;
                }

                const RenderContext::Impl::ObjectConstants push = {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0};
                SubmitDrawInstanced(passCtx.encoder, drawCmd, static_cast<uint32_t>(i), push, impl.MeshShadingActive());
            }
        });
}

}
