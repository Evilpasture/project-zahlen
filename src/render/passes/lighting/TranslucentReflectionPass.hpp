// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// The same reflection composite run against the translucent GBuffer instead of
// the opaque one: it reads the translucent normal and depth targets rather than
// the main depth, and writes the translucent lighting target that the
// `OpaqueSceneCopyPass`/`ForwardPass` pair composites over.
struct TranslucentReflectionPass: Vk::RenderPass<
                                      "TransReflection", Vk::ShaderRead<Res_SceneColor>, Vk::ShaderRead<Res_TransNorm>, Vk::ShaderRead<Res_TransDepth>,
                                      Vk::ShaderRead<Res_Clearcoat>, Vk::ShaderRead<Res_TransSheen>, Vk::ShaderRead<Res_TransAnisotropy>, Vk::ShaderRead<Res_Lighting>, Vk::ShaderRead<Res_ShadowMap>,
                                      Vk::ShaderRead<Res_ShadowAtlas>, Vk::ShaderReadGeneral<Res_VoxelResolved>, Vk::ShaderRead<Res_RtrHalf>,
                                      Vk::ColorWrite<Res_TransLighting>> {
    RenderContext::Impl&                 impl;
    GeneratedGpu::ScenePassPushConstants pc {};

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

}
