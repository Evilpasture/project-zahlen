// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Depth and normals for forward-only (translucent) geometry, in a target pair
// of its own so the composite pass can resolve refractions against what is
// behind a surface. It runs before the translucent lighting resolve and clears
// both of its targets.
struct TranslucentPrePass: Vk::RenderPass<"TransPrePass", Vk::ColorWrite<Res_TransNorm>, Vk::DepthStencilWrite<Res_TransDepth>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
