// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/passes/forward/TranslucentPrePass.hpp"
#include "render/Renderer.hpp"
#include "render/passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void TranslucentPrePass::operator()(Vk::RasterPassContextBase& pass) const noexcept {
    // The graph opened rendering on the declared translucent targets with the
    // load op the state table chose -- CLEAR, because nothing has written them
    // yet this frame -- and with the clear values from ClearColorOf<Tag> for
    // each one. This pass states its viewport and draws.
    pass.StateViewport(frame.viewport);

    Vk::CommandEncoder encoder(pass.Cmd());

    for (size_t i = 0; i < frame.renderer.queues.Draws().size(); ++i) {
        const auto& drawCmd = frame.renderer.queues.Draws()[i];
        if (!IsForwardOnly(drawCmd.instanceData.flags) || drawCmd.prePassMaterial == nullptr || drawCmd.prePassMaterial->pipeline == VK_NULL_HANDLE) {
            continue;
        }
        const ObjectConstants push = {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0};

        SubmitDrawInstanced(encoder, drawCmd, static_cast<uint32_t>(i), push, frame.meshShading, drawCmd.prePassMaterial->pipeline);
    }
}

} // namespace ZHLN::Passes
