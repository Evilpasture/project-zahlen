// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

template <typename SurfaceTag, typename OutputTag, Vk::ResourceName PassName>
struct ReflectionPass:
    Vk::RenderPass<
        PassName,
        Vk::ShaderRead<Res_SceneColor>,
        Vk::ShaderRead<typename SurfaceTag::Normals>,
        Vk::ShaderRead<typename SurfaceTag::Depth>,
        Vk::ShaderRead<Res_HiZ>,
        Vk::ShaderRead<Res_LightingGI>,
        Vk::ShaderRead<Res_LightingAo>,
        Vk::ShaderRead<Res_ShadowMap>,
        Vk::ShaderRead<Res_ShadowAtlas>,
        Vk::ShaderReadGeneral<Res_VoxelResolved>,
        Vk::ShaderRead<Res_RcgiProbes>,
        Vk::ShaderRead<Res_RcgiCascadeA>,
        Vk::ColorWrite<OutputTag>> {
    RenderFrameContext&           frame;
    Slang::ScenePassPushConstants pc {};

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept {
        Slang::ScenePassPushConstants push = pc;
        push.translucentSurface            = SurfaceTag::kTranslucentSurface;

        // Gate Tier 3 (roughness > 0.40) Radiance Cascade directional cone specular
        // strictly on live RCGI (`enableDiffuseGI && giIntensity > 0 && tlas != nullptr`),
        // keeping `pc.giIntensity` untouched for `lighting.slang` and keeping rough
        // surfaces bit-invariant when `enableRTR` toggles alone.
        const uint32_t fIdx     = frame.presenter.frameIndex;
        const bool     rcgiLive = (SurfaceTag::kTranslucentSurface == 0) && frame.ctx.RayTracingSupported() && frame.settings.rayTracing.enableDiffuseGI &&
                              frame.settings.post.giIntensity > 0.0f && frame.settings.environment.fullBright == 0 &&
                              frame.frames.tlasAddresses[fIdx] != nullptr;
        push.giIntensity = rcgiLive ? frame.settings.post.giIntensity : 0.0f;

        frame.reflectionPipeline.Dispatch(ctx, SurfaceTag::Resolve(frame), push);
    }
};

} // namespace ZHLN::Passes
