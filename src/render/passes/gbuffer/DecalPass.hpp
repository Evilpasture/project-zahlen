// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Projects deferred decals onto the GBuffer: a box is drawn per decal and the
// fragment shader reconstructs position from depth to reject what is outside
// it. It writes albedo and normal/roughness over the base pass's output and
// reads depth read-only, so it is a plain automatic render pass -- the graph
// opens it, hands over a context, and closes it.
struct DecalPass: Vk::RenderPass<"DecalPass", Vk::ShaderRead<Res_Depth>, Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_NormRough>> {
    RenderContext::Impl& impl;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

}
