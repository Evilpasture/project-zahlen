// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Assigns lights to the view's froxel grid. It reads and writes only storage
// buffers, so it declares no image usage: the graph still gives it a hazard
// slot and a dispatch point, it just has no barriers to emit.
struct ClusterCullingPass: Vk::RenderPass<"ClusterCulling"> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
