// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/passes/lighting/EmissivePass.hpp"
#include "render/Renderer.hpp"
#include "render/passes/SceneDraw.hpp"
#include "render/passes/forward/ForwardResources.hpp"
#include <ShaderBindings.hpp>
#include <cstddef>
#include <cstdint>

namespace ZHLN::Passes {

void EmissivePass::operator()(Vk::RasterPassContextBase& pass) const noexcept {
    if (!forward.emissive.Valid()) {
        return;
    }

    // The graph opened rendering on ColorWriteRGB<Res_LightingGI> +
    // DepthStencilWrite<Res_Depth> with loads from the state table; this pass
    // only states its viewport and draws what has deferred emission.
    pass.StateViewport(frame.viewport);

    auto&              ctx = frame;
    Vk::CommandEncoder encoder(pass.Cmd());
    {
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
    }
}

} // namespace ZHLN::Passes
