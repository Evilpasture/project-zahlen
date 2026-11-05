// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Rendering.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>

namespace ZHLN {
struct CullingResources;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct ClusterCullingPass: VkD3D::RenderPass<"ClusterCulling"> {
    CullingResources&   cull;
    RenderFrame&        frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
