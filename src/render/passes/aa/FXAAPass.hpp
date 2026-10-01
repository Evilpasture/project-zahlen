// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Fast approximate anti-aliasing: a single luma-edge pass with no history and
// no velocity, which is why it can run where TAA cannot -- on a scene the
// camera has just teleported, or on a render-to-texture destination.
struct FXAAPass: Vk::RenderPass<"FXAA", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ColorWrite<Res_AccumCurrent>> {
    RenderContext::Impl& impl;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

}
