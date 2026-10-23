// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "graph/RenderGraph.hpp"
#include "passes/SceneDrawResources.hpp"
#include "passes/culling/CullingResources.hpp"

namespace ZHLN::Passes {

struct GBufferResolvePass:
    Vk::RenderPass<
        "MainPass2",
        Vk::ColorWrite<Res_SceneColor>,
        Vk::ColorWrite<Res_Velocity>,
        Vk::ColorWrite<Res_NormRough>,
        Vk::ColorWrite<Res_BakedMask>,
        Vk::DepthStencilWrite<Res_Depth>,
        Vk::ComputeRead<Res_HiZ>> {
    CullingResources&            cull;
    const CsgResources&          csg;
    const MeshParticlePipelines& particles;
    RenderFrameContext&          frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
