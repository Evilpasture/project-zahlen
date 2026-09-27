// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Temporal anti-aliasing: blends this frame's HDR color against the resolved
// history buffer using per-pixel velocity to reproject, so the jitter in the
// projection matrix becomes sub-pixel detail instead of shimmer.
//
// The history is the previous frame's accumulation buffer, which is why the
// graph sees this pass read `Res_AccumCurr` and write `Res_AccumNext`: the two
// are the same pair of images swapped at `EndFrame`.
struct TAAPass: Vk::RenderPass<
                    "TAA", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ShaderRead<Res_Velocity>, Vk::ShaderRead<Res_Depth>, Vk::ColorWrite<Res_AccumNext>,
                    Vk::ShaderRead<Res_AccumCurr>> {
    RenderContext::Impl& impl;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

}
