// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "PassPayloads.hpp"
#include "graph/RenderGraph.hpp"
#include "passes/forward/ForwardResources.hpp"

namespace ZHLN::Passes {

// Compact deferred materials cannot store an emissive attachment. Their
// material emission is sampled a second time and additively rasterized into
// HDR LightingGI after GI composition, before reflection reads that target.
struct EmissivePass:
    Vk::RenderPass<"Emissive", Vk::ColorWrite<Res_LightingGI>, Vk::DepthStencilWrite<Res_Depth>> {
    using PushConstants = ObjectConstants;

    const ForwardResources& forward;
    RenderFrameContext&     frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
