// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Rendering.hpp"
#include "graph/RenderPassDecl.hpp"

namespace ZHLN {
struct CullingResources;
struct RenderFrameContext;
} // namespace ZHLN

namespace ZHLN::Passes {

struct ClusterCullingPass: Vk::RenderPass<"ClusterCulling"> {
    CullingResources&   cull;
    RenderFrameContext& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
