// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct DdgiUpdateAtlasPass: Vk::RenderPass<
                                "DdgiUpdateAtlas", Vk::ComputeRead<Res_DdgiRays>, Vk::ComputeWrite<Res_DdgiIrradiance>, Vk::ComputeWrite<Res_DdgiDistance>> {
    RenderFrameContext&   impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
