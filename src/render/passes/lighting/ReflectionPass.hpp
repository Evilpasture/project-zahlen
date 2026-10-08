// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"
#include "GBufferSurface.hpp"

namespace ZHLN::Passes {

template <typename SurfaceTag, typename OutputTag, Vk::ResourceName PassName>
struct ReflectionPass: Vk::RenderPass<
                          PassName,
                          Vk::ShaderRead<Res_SceneColor>,
                          Vk::ShaderRead<typename SurfaceTag::Normals>,
                          Vk::ShaderRead<Res_Clearcoat>,
                          Vk::ShaderRead<typename SurfaceTag::Sheen>,
                          Vk::ShaderRead<typename SurfaceTag::Anisotropy>,
                          Vk::ShaderRead<typename SurfaceTag::Depth>,
                          Vk::ShaderRead<Res_Lighting>,
                          Vk::ShaderRead<Res_ShadowMap>,
                          Vk::ShaderRead<Res_ShadowAtlas>,
                          Vk::ShaderReadGeneral<Res_VoxelResolved>,
                          Vk::ShaderRead<Res_RtrHalf>,
                          Vk::ColorWrite<OutputTag>> {
    RenderContext::Impl&                 impl;
    GeneratedGpu::ScenePassPushConstants pc {};

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept {
        impl.reflectionPipeline.Dispatch(ctx, SurfaceTag::Resolve(impl), pc);
    }
};

}
