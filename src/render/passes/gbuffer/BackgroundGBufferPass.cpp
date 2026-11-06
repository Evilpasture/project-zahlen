// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/passes/gbuffer/BackgroundGBufferPass.hpp"
#include "render/Renderer.hpp"
#include "render/passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// Graph scope: the graph opens rendering on the declared G-buffer targets.
// GBufferBasePass ran first and cleared the shared targets; the state table
// therefore loads them here, and this pass stamps the skybox scenery. That
// scenery is ordinary geometry in the same world, so it is ordered by plain
// depth: it fills the pixels the map left empty and is occluded wherever the
// map is nearer. (It used to be forced behind the world by a second depth
// band; that split is retired.)
void BackgroundGBufferPass::operator()(Vk::RasterPassContextBase& pass) const noexcept {
    pass.StateViewport(frame.viewport);

    Vk::CommandEncoder encoder(pass.Cmd());

    for (size_t i = 0; i < frame.renderer.queues.Draws().size(); ++i) {
        const auto& drawCmd = frame.renderer.queues.Draws()[i];
        if ((drawCmd.flags & DrawFlags::Background) == DrawFlags::None || !IsVisibleIn(drawCmd.flags, RenderPassType::Main) ||
            IsForwardOnly(drawCmd.instanceData.flags) || drawCmd.material == nullptr || drawCmd.material->pipeline == VK_NULL_HANDLE) {
            continue;
        }

        const ObjectConstants push = {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0};

        // No pipeline override: this pass presents the same G-buffer contract
        // as GBufferBasePass, so it must resolve the pipeline the same way.
        // Passing drawCmd.material->pipeline here would pin the background
        // layer to the classic vertex path and drop it off the meshlet path on
        // GPUs that use it, for no gain -- SubmitDrawInstanced already falls
        // back to that pipeline.
        SubmitDrawInstanced(encoder, drawCmd, static_cast<uint32_t>(i), push, frame.meshShading);
    }
}

} // namespace ZHLN::Passes
