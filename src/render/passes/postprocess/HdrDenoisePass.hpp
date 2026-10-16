// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "TargetManager.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct HdrDenoisePass:
    Vk::RenderPass<
        "HdrDenoise",
        Vk::ComputeWrite<Res_HdrSceneColor>,
        Vk::ComputeWrite<Res_DenoiseA>,
        Vk::ComputeWrite<Res_DenoiseB>,
        Vk::ComputeRead<Res_Depth>,
        Vk::ComputeRead<Res_NormRough>> {
    RenderFrameContext& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
