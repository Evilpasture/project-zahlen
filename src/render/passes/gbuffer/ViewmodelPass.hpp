// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderTargetTags.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>

namespace ZHLN {
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct ViewmodelPass:
    VkD3D::RenderPass<
        "Viewmodel",
        VkD3D::ColorWrite<Res_SceneColor>,
        VkD3D::ColorWrite<Res_Velocity>,
        VkD3D::ColorWrite<Res_NormRough>,
        VkD3D::ColorWrite<Res_BakedMask>,
        VkD3D::DepthStencilWrite<Res_Depth>> {
    RenderFrame& frame;

    void operator()(Vk::RasterPassContextBase& pass) const noexcept;
};

} // namespace ZHLN::Passes
