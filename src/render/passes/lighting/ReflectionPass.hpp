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
        Vk::ShaderRead<Res_LightingGI>,
        Vk::ShaderRead<Res_ShadowMap>,
        Vk::ShaderRead<Res_ShadowAtlas>,
        Vk::ShaderReadGeneral<Res_VoxelResolved>,
        Vk::ShaderRead<Res_RtrHalf>,
        Vk::ShaderRead<Res_DdgiIrradiance>,
        Vk::ShaderRead<Res_DdgiDistance>,
        Vk::ColorWrite<OutputTag>> {
    RenderFrameContext&           frame;
    Slang::ScenePassPushConstants pc {};

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept {
        Slang::ScenePassPushConstants push = pc;
        push.translucentSurface = SurfaceTag::kTranslucentSurface;
        frame.reflectionPipeline.Dispatch(ctx, SurfaceTag::Resolve(frame), push);
    }
};

} // namespace ZHLN::Passes
