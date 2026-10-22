// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/forward/TranslucentPrePass.hpp"
#include "passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void TranslucentPrePass::operator()(VkCommandBuffer cmd) const noexcept {
    PassContext passCtx(cmd, frame);
    const auto  sceneVp = frame.EffectiveViewport();

    const auto norm_att  = Vk::Assume<Vk::ColorWrite<Res_TransNorm>>(frame.graphResources.transNormalBuffer);
    const auto aniso_att = Vk::Assume<Vk::ColorWrite<Res_TransAnisotropy>>(frame.graphResources.transAnisotropyBuffer);
    const auto sheen_att = Vk::Assume<Vk::ColorWrite<Res_TransSheen>>(frame.graphResources.transSheenBuffer);
    const auto depth_att = Vk::Assume<Vk::DepthStencilWrite<Res_TransDepth>>(frame.graphResources.transDepthBuffer);

    Vk::DynamicPass(norm_att.Extent())
        .Viewport(sceneVp)
        .ClearColor(norm_att, kClearColorNormalRoughness)
        .ClearColor(aniso_att, kClearColorAnisotropy)
        .ClearColor(sheen_att, kClearColorSheen)
        .ClearDepth(depth_att, kClearDepthValue)
        .Execute(cmd, [&]() {
            for (size_t i = 0; i < frame.queues.Draws().size(); ++i) {
                const auto& drawCmd = frame.queues.Draws()[i];
                if (!IsForwardOnly(drawCmd.instanceData.flags) || drawCmd.prePassMaterial == nullptr || drawCmd.prePassMaterial->pipeline == VK_NULL_HANDLE) {
                    continue;
                }
                const ObjectConstants push = {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0};

                SubmitDrawInstanced(passCtx.encoder, drawCmd, static_cast<uint32_t>(i), push, frame.MeshShadingActive(), drawCmd.prePassMaterial->pipeline);
            }
        });
}

} // namespace ZHLN::Passes
