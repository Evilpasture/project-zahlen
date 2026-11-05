// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderTargetTags.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>

namespace ZHLN {
struct AntiAliasingResources;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

// Depth and normal/roughness are read by the hybrid edge detector so that
// silhouettes and hard creases survive even where HDR luma is locally flat.
struct SmaaEdgePass:
    VkD3D::RenderPass<"SmaaEdge", VkD3D::ShaderRead<Res_HdrSceneColor>, VkD3D::ShaderRead<Res_Depth>, VkD3D::ShaderRead<Res_NormRough>,
                   VkD3D::ColorWrite<Res_SmaaEdge>> {
    AntiAliasingResources& aa;
    RenderFrame&           frame;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

struct SmaaWeightPass: VkD3D::RenderPass<"SmaaWeight", VkD3D::ShaderRead<Res_SmaaEdge>, VkD3D::ColorWrite<Res_SmaaWeight>> {
    AntiAliasingResources& aa;
    RenderFrame&           frame;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

struct SmaaBlendPass: VkD3D::RenderPass<"SmaaBlend", VkD3D::ShaderRead<Res_HdrSceneColor>, VkD3D::ShaderRead<Res_SmaaWeight>, VkD3D::ColorWrite<Res_AccumCurrent>> {
    AntiAliasingResources& aa;
    RenderFrame&           frame;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

} // namespace ZHLN::Passes
