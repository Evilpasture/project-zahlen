// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/forward/TranslucentPrePass.hpp"
#include "passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void TranslucentPrePass::operator()(VkCommandBuffer cmd) const noexcept {
    impl.BindHeapsAndPushFrame(cmd);

    PassContext passCtx(cmd, impl);
    const auto  sceneVp = impl.EffectiveViewport();

    const auto norm_att  = Vk::Assume<Vk::ColorWrite<Res_TransNorm>>(impl.graphResources.transNormalBuffer);
    const auto depth_att = Vk::Assume<Vk::DepthStencilWrite<Res_TransDepth>>(impl.graphResources.transDepthBuffer);

    Vk::DynamicPass(norm_att.extent)
        .Viewport(sceneVp)
        .AddColor(norm_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorNormalRoughness)
        .AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearDepthValue)
        .Execute(cmd, [&]() {
            for (size_t i = 0; i < impl.queues.Draws().size(); ++i) {
                const auto& drawCmd = impl.queues.Draws()[i];

                if ((drawCmd.instanceData.flags & 0xFF) != 2) {
                    continue;
                }

                if (drawCmd.prePassMaterial == nullptr || !drawCmd.prePassMaterial->pipeline.Valid()) {
                    continue;
                }

                const RenderContext::Impl::ObjectConstants push = {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0};

                SubmitDrawInstanced(
                    passCtx.encoder, drawCmd, static_cast<uint32_t>(i), push, impl.MeshShadingActive(), drawCmd.prePassMaterial->pipeline.Get(),
                    drawCmd.prePassMaterial->layout
                );
            }
        });
}

}
