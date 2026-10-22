// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/gbuffer/DecalPass.hpp"
#include "RenderInternal.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

using DecalPushConstants = Slang::DecalPushConstants;
static_assert(GpuAbi::ScenePassPayload<DecalPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void DecalPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    auto* const cmd = ctx.Cmd();
    if (!decals.pass.Valid() || frame.queues.Decals().empty()) {
        return;
    }

    PassContext passCtx(cmd, frame);
    passCtx.encoder.BindPipeline(decals.pass.Get());

    const auto invViewProj = frame.unjittered_view_proj.Inversed();

    for (const auto& decalCmd: frame.queues.Decals()) {
        const DecalPushConstants decalPC {
            .worldMatrix = decalCmd.transform,
            .clipToLocal = decalCmd.invTransform * invViewProj,
            .albedoIndex = decalCmd.albedoIndex,
            .normalIndex = decalCmd.normalIndex,
            .roughness   = decalCmd.roughness,
            .metallic    = decalCmd.metallic
        };

        passCtx.encoder.BindPipeline(decals.pass.Get());
        passCtx.encoder.DrawHeap<Shaders::Modules::DecalVS, Shaders::Modules::DecalPS>(36, 1, decalPC);
    }
}

} // namespace ZHLN::Passes
