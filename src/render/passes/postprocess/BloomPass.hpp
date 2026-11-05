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

struct BloomPass:
    VkD3D::RenderPass<
        "BloomKawase",
        VkD3D::ComputeRead<Res_HdrSceneColor>,
        VkD3D::ComputeWrite<Res_BloomThresh>,
        VkD3D::ComputeWrite<Res_BloomDown1>,
        VkD3D::ComputeWrite<Res_BloomDown2>,
        VkD3D::ComputeWrite<Res_BloomDown3>,
        VkD3D::ComputeWrite<Res_BloomUp2>,
        VkD3D::ComputeWrite<Res_BloomUp1>,
        VkD3D::ComputeWrite<Res_BloomFinal>> {
    RenderFrame& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
