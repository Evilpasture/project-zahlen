// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderFrame.hpp"
#include "Renderer.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>

namespace ZHLN::Passes {

template <typename SurfaceTag, typename OutputTag, VkD3D::ResourceName PassName>
struct ReflectionPass:
    VkD3D::RenderPass<
        PassName,
        VkD3D::ShaderRead<Res_SceneColor>,
        VkD3D::ShaderRead<typename SurfaceTag::Normals>,
        VkD3D::ShaderRead<typename SurfaceTag::Depth>,
        VkD3D::ShaderRead<Res_LightingGI>,
        VkD3D::ShaderRead<Res_LightingAo>,
        VkD3D::ShaderRead<Res_ShadowMap>,
        VkD3D::ShaderRead<Res_ShadowAtlas>,
        VkD3D::ShaderReadGeneral<Res_VoxelInt>,
        VkD3D::ShaderRead<Res_RcgiProbes>,
        VkD3D::ShaderRead<Res_RcgiCascadeA>,
        VkD3D::ColorWrite<OutputTag>> {
    RenderFrame&                  frame;
    Slang::ScenePassPushConstants pc {};

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept {
        Slang::ScenePassPushConstants push = pc;
        push.translucentSurface            = SurfaceTag::kTranslucentSurface;

        // Gate Tier 3 (roughness > 0.40) Radiance Cascade directional cone specular
        // strictly on live RCGI (`enableDiffuseGI && giIntensity > 0 && tlas != nullptr`),
        // keeping `pc.giIntensity` untouched for `lighting.slang` and keeping rough
        // surfaces bit-invariant when `enableRTR` toggles alone.
        const bool rcgiLive = (SurfaceTag::kTranslucentSurface == 0) && frame.renderer.ctx.RayTracingSupported() && frame.settings.rayTracing.enableDiffuseGI &&
                              frame.settings.post.giIntensity > 0.0f && frame.settings.environment.fullBright == 0 && frame.tlasAddresses != nullptr;
        push.giIntensity = rcgiLive ? frame.settings.post.giIntensity : 0.0f;

        frame.renderer.reflectionPipeline.Dispatch(frame, ctx, SurfaceTag::Resolve(frame), push);
    }
};

} // namespace ZHLN::Passes
