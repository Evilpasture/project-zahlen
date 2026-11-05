// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PassPayloads.hpp"
#include "RenderTargetTags.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>

namespace ZHLN {
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

// Source draws its 3D skybox as a second world behind the main one, and keeps
// it behind the map with a depth-range remap: the world's clip depth occupies
// [0, kSkyDepthSplit] and the background layer is compressed into
// [kSkyDepthSplit, 1] (BandedClipDepth in common.slang), so no background
// fragment can ever win a depth test against world geometry -- "the map wins
// every pixel it covers" is enforced by the rasterizer, wherever the skybox
// model sits. This pass runs after GBufferBasePass and draws the background
// layer into the same G-buffer and the same depth target: the band gives its
// pieces self-occlusion and the ordinary clustered shading path, and the depth
// test confines them to the pixels the world left empty. Pixels no layer drew
// keep the cleared depth and are shaded from the environment.
struct BackgroundGBufferPass:
    VkD3D::RenderPass<
        "SkyboxGBuffer",
        VkD3D::ColorWrite<Res_SceneColor>,
        VkD3D::ColorWrite<Res_Velocity>,
        VkD3D::ColorWrite<Res_NormRough>,
        VkD3D::ColorWrite<Res_BakedMask>,
        VkD3D::DepthStencilWrite<Res_Depth>> {
    using PushConstants = ObjectConstants;

    RenderFrame& frame;

    void operator()(Vk::RasterPassContextBase& pass) const noexcept;
};

} // namespace ZHLN::Passes
