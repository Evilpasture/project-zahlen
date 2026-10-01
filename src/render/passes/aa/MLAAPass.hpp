// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Morphological anti-aliasing: finds the discontinuity lines in the luma
// buffer and blends across them by the area each side covers. Like FXAA it is
// history-free, and it is the sharpest of the three on long straight edges.
struct MLAAPass: Vk::RenderPass<"MLAA", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ColorWrite<Res_AccumCurrent>> {
    RenderContext::Impl& impl;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

}
