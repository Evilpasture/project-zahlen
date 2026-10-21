// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/gbuffer/ViewmodelPass.hpp"
#include "passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void ViewmodelPass::operator()(VkCommandBuffer cmd) const noexcept {
    using enum DrawFlags;
    const auto& draws = frame.queues.Draws();
    if (!std::ranges::any_of(draws, [](const auto& drawCmd) { return (drawCmd.flags & Viewmodel) != None && !IsForwardOnly(drawCmd.instanceData.flags); })) {
        return;
    }

    frame.BindHeapsAndPushFrame(cmd);

    PassContext passCtx(cmd, frame);
    const auto  in = GBufferSceneTargets(frame);

    Vk::DynamicPass(in.sceneColor.Extent())
        .Viewport(frame.EffectiveViewport())
        .LoadColor(in.sceneColor)
        .LoadColor(in.velocity)
        .LoadColor(in.normRough)
        .LoadColor(in.bakedMask)
        .LoadDepth(in.depth)
        .Execute(cmd, [&]() {
            for (size_t i = 0; i < frame.queues.Draws().size(); ++i) {
                const auto& drawCmd = frame.queues.Draws()[i];

                if ((drawCmd.flags & Viewmodel) == None || IsForwardOnly(drawCmd.instanceData.flags) || drawCmd.material->pipeline == VK_NULL_HANDLE) {
                    continue;
                }

                const ObjectConstants push = {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0};
                SubmitDrawInstanced(passCtx.encoder, drawCmd, static_cast<uint32_t>(i), push, frame.MeshShadingActive());
            }
        });
}

} // namespace ZHLN::Passes
