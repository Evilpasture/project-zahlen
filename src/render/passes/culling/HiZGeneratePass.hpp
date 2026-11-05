// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderTargetTags.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>

namespace ZHLN {
struct CullingResources;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct HiZGeneratePass: VkD3D::RenderPass<"HiZGenerate", VkD3D::ShaderRead<Res_Depth>, VkD3D::ComputeWrite<Res_HiZ>> {
    CullingResources&   cull;
    RenderFrame&        frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
