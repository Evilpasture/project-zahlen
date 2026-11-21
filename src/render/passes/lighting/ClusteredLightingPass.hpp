// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "render/PassPayloads.hpp"
#include "render/RenderTargetTags.hpp"
#include "vulkan/graph/RenderPassDecl.hpp"

namespace ZHLN {
struct PostProcessResources;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct ClusteredLightingPass:
    Vk::RenderPass<
        "Lighting",
        Vk::FragmentAndComputeRead<Res_SceneColor>,
        Vk::FragmentAndComputeRead<Res_NormRough>,
        Vk::FragmentAndComputeRead<Res_BakedMask>,
        Vk::FragmentAndComputeRead<Res_Depth>,
        // The sun's mask, resolved by SunShadowMaskPass (which is what reads
        // Res_ShadowMap now). Cascade PCSS and the contact ray do not run here;
        // this pass fetches their answer.
        Vk::FragmentAndComputeRead<Res_SunShadowMask>,
        Vk::ShaderRead<Res_ShadowAtlas>,
        Vk::ShaderRead<Res_Ao>,
        Vk::ColorWrite<Res_Lighting>,
        Vk::ColorWrite<Res_LightingAo>> {
    const PostProcessResources&   post;
    RenderFrame&                  frame;
    Slang::ScenePassPushConstants pc {};

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

} // namespace ZHLN::Passes
