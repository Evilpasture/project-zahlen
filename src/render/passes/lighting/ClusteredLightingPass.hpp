// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PassPayloads.hpp"
#include "RenderTargetTags.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>

namespace ZHLN {
struct PostProcessResources;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct ClusteredLightingPass:
    VkD3D::RenderPass<
        "Lighting",
        VkD3D::FragmentAndComputeRead<Res_SceneColor>,
        VkD3D::FragmentAndComputeRead<Res_NormRough>,
        VkD3D::FragmentAndComputeRead<Res_BakedMask>,
        VkD3D::FragmentAndComputeRead<Res_Depth>,
        VkD3D::ShaderRead<Res_ShadowMap>,
        VkD3D::ShaderRead<Res_ShadowAtlas>,
        VkD3D::ShaderRead<Res_Ao>,
        VkD3D::ColorWrite<Res_Lighting>,
        VkD3D::ColorWrite<Res_LightingAo>> {
    const PostProcessResources&   post;
    RenderFrame&                  frame;
    Slang::ScenePassPushConstants pc {};

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

} // namespace ZHLN::Passes
