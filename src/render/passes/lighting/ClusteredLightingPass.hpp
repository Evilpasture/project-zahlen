// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Deferred lighting: evaluates the clustered light list against the GBuffer
// and writes the shaded radiance the reflection pass composes on top of.
//
// It reads everything the GBuffer passes wrote plus the shadow maps, so it is
// the pass that turns the graph's hazard analysis into real work: nine image
// reads, each with its own barrier, before a single draw.
struct ClusteredLightingPass: Vk::RenderPass<
                                  "Lighting", Vk::ShaderRead<Res_SceneColor>, Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Emissive>,
                                  Vk::ShaderRead<Res_Clearcoat>, Vk::ShaderRead<Res_Anisotropy>, Vk::ShaderRead<Res_Depth>, Vk::ShaderRead<Res_ShadowMap>, Vk::ShaderRead<Res_ShadowAtlas>,
                                  Vk::ShaderRead<Res_Ao>, Vk::ColorWrite<Res_Lighting>> {
    RenderContext::Impl&                 impl;
    GeneratedGpu::ScenePassPushConstants pc {};

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

}
