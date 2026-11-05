// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PassPayloads.hpp"
#include "RenderTargetTags.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>

namespace ZHLN {
struct CullingResources;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct GBufferBasePass:
    VkD3D::RenderPass<
        "MainPass1",
        VkD3D::ColorWrite<Res_SceneColor>,
        VkD3D::ColorWrite<Res_Velocity>,
        VkD3D::ColorWrite<Res_NormRough>,
        VkD3D::ColorWrite<Res_BakedMask>,
        VkD3D::DepthStencilWrite<Res_Depth>> {
    using PushConstants = ObjectConstants;

    CullingResources&   cull;
    RenderFrame&        frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
