// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "TargetManager.hpp"
#include "graph/RenderGraph.hpp"
#include "passes/postprocess/PostProcessResources.hpp"

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
        Vk::ShaderRead<Res_RtShadowMask>,
        Vk::FragmentAndComputeRead<Res_DdgiIrradiance>,
        Vk::FragmentAndComputeRead<Res_DdgiDistance>,
        Vk::ColorWrite<Res_Lighting>,
        Vk::ColorWrite<Res_LightingAo>> {
    const PostProcessResources&   post;
    RenderFrameContext&           frame;
    Slang::ScenePassPushConstants pc {};

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

} // namespace ZHLN::Passes
