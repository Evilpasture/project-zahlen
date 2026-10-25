// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "TargetManager.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct BloomPass:
    Vk::RenderPass<
        "BloomKawase",
        Vk::ComputeRead<Res_HdrSceneColor>,
        Vk::ComputeWrite<Res_BloomThresh>,
        Vk::ComputeWrite<Res_BloomDown1>,
        Vk::ComputeWrite<Res_BloomDown2>,
        Vk::ComputeWrite<Res_BloomDown3>,
        Vk::ComputeWrite<Res_BloomUp2>,
        Vk::ComputeWrite<Res_BloomUp1>,
        Vk::ComputeWrite<Res_BloomFinal>> {
    RenderFrameContext& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
