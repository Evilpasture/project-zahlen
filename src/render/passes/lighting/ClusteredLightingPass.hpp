// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PassPayloads.hpp"
#include "RenderTargetTags.hpp"
#include "graph/RenderPassDecl.hpp"

namespace ZHLN {
struct PostProcessResources;
struct RenderFrameContext;
} // namespace ZHLN

namespace ZHLN::Passes {

struct ClusteredLightingPass:
    Vk::RenderPass<
        "Lighting",
        Vk::FragmentAndComputeRead<Res_SceneColor>,
        Vk::FragmentAndComputeRead<Res_NormRough>,
        Vk::FragmentAndComputeRead<Res_BakedMask>,
        Vk::FragmentAndComputeRead<Res_Depth>,
        Vk::ShaderRead<Res_ShadowMap>,
        Vk::ShaderRead<Res_ShadowAtlas>,
        Vk::ShaderRead<Res_Ao>,
        Vk::ColorWrite<Res_Lighting>,
        Vk::ColorWrite<Res_LightingAo>> {
    const PostProcessResources&   post;
    RenderFrameContext&           frame;
    Slang::ScenePassPushConstants pc {};

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

} // namespace ZHLN::Passes
