// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "TargetManager.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct RtgiDenoisePass:
    Vk::RenderPass<
        "RtgiDenoise",
        Vk::ComputeWrite<Res_RtgiHalf>,
        Vk::ComputeWrite<Res_RtgiDenoiseA>,
        Vk::ComputeWrite<Res_RtgiDenoiseB>,
        Vk::ComputeRead<Res_RtgiHistory>,
        Vk::ComputeRead<Res_Velocity>,
        Vk::ComputeRead<Res_Depth>,
        Vk::ComputeRead<Res_NormRough>> {
    RenderFrameContext& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
