// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Subpixel morphological anti-aliasing, in the three sub-passes the original
// technique splits into:
//
//   1. `SmaaEdgePass`   -- edge detection, writing a two-channel edge texture;
//   2. `SmaaWeightPass` -- blending weights, looked up in the area and search
//                          LUTs that `PostProcessFeature` baked at startup;
//   3. `SmaaBlendPass`  -- the neighborhood blend that mixes the color buffer
//                          by those weights.
//
// They are three graph passes rather than one because the second depends on
// every texel of the first and the third on every texel of the second: the
// graph has to barrier between them, and it can only do that if they are
// separate nodes. They still fork nowhere and run strictly in this order.

struct SmaaEdgePass: Vk::RenderPass<"SmaaEdge", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ColorWrite<Res_SmaaEdge>> {
    RenderContext::Impl& impl;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

struct SmaaWeightPass: Vk::RenderPass<"SmaaWeight", Vk::ShaderRead<Res_SmaaEdge>, Vk::ColorWrite<Res_SmaaWeight>> {
    RenderContext::Impl& impl;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

struct SmaaBlendPass: Vk::RenderPass<"SmaaBlend", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ShaderRead<Res_SmaaWeight>, Vk::ColorWrite<Res_AccumCurrent>> {
    RenderContext::Impl& impl;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

}
