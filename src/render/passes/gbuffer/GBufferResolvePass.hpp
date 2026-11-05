// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderTargetTags.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>

namespace ZHLN {
struct CsgResources;
struct CullingResources;
struct MeshParticlePipelines;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct GBufferResolvePass:
    VkD3D::RenderPass<
        "MainPass2",
        VkD3D::ColorWrite<Res_SceneColor>,
        VkD3D::ColorWrite<Res_Velocity>,
        VkD3D::ColorWrite<Res_NormRough>,
        VkD3D::ColorWrite<Res_BakedMask>,
        VkD3D::DepthStencilWrite<Res_Depth>,
        VkD3D::ComputeRead<Res_HiZ>> {
    CullingResources&            cull;
    const CsgResources&          csg;
    const MeshParticlePipelines& particles;
    RenderFrame&                 frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
