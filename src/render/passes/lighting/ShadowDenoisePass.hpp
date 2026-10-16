// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "TargetManager.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct ShadowDenoisePass:
    Vk::RenderPass<
        "ShadowDenoise",
        Vk::ComputeWrite<Res_RtShadowMask>,
        Vk::ComputeWrite<Res_ShadowDenoiseA>,
        Vk::ComputeWrite<Res_ShadowDenoiseB>,
        Vk::ComputeRead<Res_RtShadowHistory>,
        Vk::ComputeRead<Res_Velocity>,
        Vk::ComputeRead<Res_Depth>,
        Vk::ComputeRead<Res_NormRough>> {
    RenderFrameContext& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
