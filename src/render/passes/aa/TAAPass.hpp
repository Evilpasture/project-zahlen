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
// The graph reads the prior accumulation and writes this frame's result into
// the other image. `PingPong::Swap()` exchanges their roles at `EndFrame`. The
// renderer waits for the previous frame's fence before reusing either image.
struct TAAPass: Vk::RenderPass<
                    "TAA", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ShaderRead<Res_Velocity>, Vk::ShaderRead<Res_Depth>, Vk::ColorWrite<Res_AccumCurrent>,
                    Vk::ShaderRead<Res_AccumPrevious>> {
    RenderContext::Impl& impl;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

}
