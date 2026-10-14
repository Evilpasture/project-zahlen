// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct TAAPass: Vk::RenderPass<
                    "TAA", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ShaderRead<Res_Velocity>, Vk::ShaderRead<Res_Depth>, Vk::ColorWrite<Res_AccumCurrent>,
                    Vk::ShaderRead<Res_AccumPrevious>> {
    AntiAliasingResources& aa;
    RenderFrameContext&    frame;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

}
