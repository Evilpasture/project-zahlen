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

struct RtgiCompositePass:
    VkD3D::RenderPass<
        "RtgiComposite",
        VkD3D::ComputeRead<Res_SceneColor>,
        VkD3D::ComputeRead<Res_Depth>,
        VkD3D::ComputeRead<Res_NormRough>,
        VkD3D::ComputeRead<Res_Lighting>,
        VkD3D::ComputeRead<Res_LightingAo>,
        VkD3D::ComputeRead<Res_RcgiProbes>,
        VkD3D::ComputeRead<Res_RcgiCascadeA>,
        VkD3D::ComputeWrite<Res_LightingGI>> {
    RenderFrame& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
