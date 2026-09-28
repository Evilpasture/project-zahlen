// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/gbuffer/DecalPass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// One decal instance: its transform, the clip-to-local matrix that
// reconstructs the object-space position from the depth buffer, and the
// material overrides it stamps.
struct DecalPushConstants {
    JPH::Mat44 worldMatrix;
    JPH::Mat44 clipToLocal;
    uint32_t   albedoIndex;
    uint32_t   normalIndex;
    float      roughness;
    float      metallic;
};
static_assert(GpuAbi::ScenePassPayload<DecalPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void DecalPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    auto cmd = ctx.Cmd();
    if (!impl.decalPipeline.Valid() || impl.queues.Decals().empty()) {
        return;
    }

    impl.BindHeapsAndPushFrame(cmd);

    PassContext passCtx(cmd, impl);
    passCtx.encoder.BindPipeline(impl.decalPipeline.Get(), impl.decalPipelineLayout);

    const JPH::Mat44 invViewProj = impl.unjittered_view_proj.Inversed();

    for (const auto& decalCmd: impl.queues.Decals()) {
        const DecalPushConstants decalPC {
            .worldMatrix = decalCmd.transform,
            .clipToLocal = decalCmd.invTransform * invViewProj,
            .albedoIndex = decalCmd.albedoIndex,
            .normalIndex = decalCmd.normalIndex,
            .roughness   = decalCmd.roughness,
            .metallic    = decalCmd.metallic
        };

        passCtx.encoder.BindPipeline(impl.decalPipeline.Get(), impl.decalPipelineLayout);
        passCtx.encoder.DrawHeap<Shaders::Modules::DecalVS, Shaders::Modules::DecalPS>(36, 1, decalPC);
    }
}

}
