// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/passes/gbuffer/GBufferResolvePass.hpp"
#include "render/Renderer.hpp"
#include "render/passes/SceneDraw.hpp"
#include "render/passes/gbuffer/GBufferCullingPass.hpp"
#include "render/passes/culling/CullingResources.hpp"
#include <ShaderBindings.hpp>
#include <algorithm>

namespace ZHLN::Passes {

namespace {

// Draws only; GBufferResolveCullingPass records the clear, barriers and dispatch
// outside the rendering scope.
void RecordGpuCulledDraws(
    RenderFrame&                 ctx,
    Vk::CommandEncoder&          encoder,
    const CsgResources&          csg,
    const MeshParticlePipelines& particles,
    std::span<const GroupRange>  groups,
    const GBufferTargets&        in
) noexcept {
    encoder.SetViewport(ctx.viewport);

    // The buffer the prologue filled; declared here because the sequence that
    // produced it now lives in GBufferResolveCullingPass.
    const auto& indirectBuf = ctx.indirectCommandsBuffersPass2;

    for (const auto& group: groups) {
        if (group.material->pipeline == VK_NULL_HANDLE) {
            continue;
        }
        encoder.DrawIndirect<Shaders::Modules::BasicVS, Shaders::Modules::BasicVSForward>(
            {
                .pipeline       = group.material->pipeline,
                .argumentBuffer = indirectBuf.Handle(),
                .offset         = Vk::DrawIndirectState::OffsetForIndex(group.start),
                .drawCount      = group.count,
            },
            ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 0}
        );
    }

    DrawCSGMeshes(ctx, encoder, csg, in.sceneColor.Extent());
    Draw3DParticles(ctx, encoder, particles);
}

void RecordCpuCulled(
    RenderFrame&                 ctx,
    Vk::CommandEncoder&          encoder,
    const CsgResources&          csg,
    const MeshParticlePipelines& particles,
    const GBufferTargets&        in
) noexcept {
    encoder.SetViewport(ctx.viewport);

    // The graph holds the scope open on the same five targets; these two draw
    // calls are all this path adds inside it.
    DrawCSGMeshes(ctx, encoder, csg, in.sceneColor.Extent());
    Draw3DParticles(ctx, encoder, particles);
}

} // namespace

void GBufferResolvePass::operator()(Vk::RasterPassContextBase& pass) const noexcept {
    // Unlike the first G-buffer pass this one is not the frame's first writer:
    // GBufferBasePass already cleared and filled these targets, so the state
    // table hands this pass LOAD instead of CLEAR -- which is exactly what the
    // manual LoadColor/LoadDepth list stated.
    pass.StateViewport(frame.viewport);

    Vk::CommandEncoder encoder(pass.Cmd());

    const auto drawCount = static_cast<uint32_t>(frame.renderer.queues.Draws().size());
    if (drawCount == 0 && frame.renderer.queues.MeshParticleEmitters().empty() && frame.renderer.queues.CsgDraws().empty()) {
        return;
    }

    const auto groups = BuildGroupRanges(frame);
    const auto in     = GBufferSceneTargets(frame);
    if (UsesGpuCulling(cull, frame, true, drawCount)) {
        RecordGpuCulledDraws(frame, encoder, csg, particles, groups, in);
    } else {
        RecordCpuCulled(frame, encoder, csg, particles, in);
    }
}

} // namespace ZHLN::Passes
