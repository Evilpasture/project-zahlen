// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct FXAAPass: Vk::RenderPass<"FXAA", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ColorWrite<Res_AccumCurrent>> {
    AntiAliasingResources& aa;
    RenderFrameContext&    frame;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

}
