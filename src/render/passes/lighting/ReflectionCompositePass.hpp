// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Composes the opaque scene's specular term: image-based lighting from the
// prefiltered environment, plus screen-space reflection and the traced
// half-resolution reflection, blended against roughness and clearcoat. Its
// output is the HDR scene color everything downstream -- translucents,
// forward draws, denoising, bloom -- works from.
struct ReflectionCompositePass: Vk::RenderPass<
                                    "Reflection", Vk::ShaderRead<Res_SceneColor>, Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Clearcoat>, Vk::ShaderRead<Res_Anisotropy>,
                                    Vk::ShaderRead<Res_Depth>, Vk::ShaderRead<Res_Lighting>, Vk::ShaderRead<Res_ShadowMap>, Vk::ShaderRead<Res_ShadowAtlas>,
                                    Vk::ShaderReadGeneral<Res_VoxelResolved>, Vk::ShaderRead<Res_RtrHalf>, Vk::ColorWrite<Res_HdrSceneColor>> {
    RenderContext::Impl&                 impl;
    GeneratedGpu::ScenePassPushConstants pc {};

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

}
