// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Depth-only shadow rendering: four cascaded sun cascades in one multiview
// pass, then the shadow atlas.
//
// The CPU-side work here is the per-slot culling that fills the indirect
// command buffer: an instance is written into the cascade slot if it is inside
// any cascade frustum, and into a light's atlas tile if it is inside that
// light's range. The pipelines and that buffer belong to `ShadowRenderer`;
// this pass is the recording of them.
//
// The atlas half is one pass over the whole image, loaded rather than cleared.
// Each tile is cleared and rasterized inside it with its own viewport, so the
// tiles `ShadowAtlas` reports as cached keep last frame's depth and cost
// nothing to keep.
struct ShadowPass: Vk::RenderPass<"MainShadow", Vk::DepthWrite<Res_ShadowMap>, Vk::DepthWrite<Res_ShadowAtlas>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
