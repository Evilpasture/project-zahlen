// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/passes/gbuffer/ViewmodelPass.hpp"
#include "render/Renderer.hpp"
#include "render/passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void ViewmodelPass::operator()(Vk::RasterPassContextBase& pass) const noexcept {
    using enum DrawFlags;
    const auto& draws = frame.renderer.queues.Draws();
    if (!std::ranges::any_of(draws, [](const auto& drawCmd) { return (drawCmd.flags & Viewmodel) != None && !IsForwardOnly(drawCmd.instanceData.flags); })) {
        return;
    }

    // The graph opened rendering on the declared G-buffer targets with loads
    // from the state table; this pass only states its viewport and draws the
    // viewmodel subset.
    pass.StateViewport(frame.viewport);

    Vk::CommandEncoder encoder(pass.Cmd());

    for (size_t i = 0; i < frame.renderer.queues.Draws().size(); ++i) {
        const auto& drawCmd = frame.renderer.queues.Draws()[i];

        if ((drawCmd.flags & Viewmodel) == None || IsForwardOnly(drawCmd.instanceData.flags) || drawCmd.material->pipeline == VK_NULL_HANDLE) {
            continue;
        }

        const ObjectConstants push = {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0};
        SubmitDrawInstanced(encoder, drawCmd, static_cast<uint32_t>(i), push, frame.meshShading);
    }
}

} // namespace ZHLN::Passes
