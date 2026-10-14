// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct RtrDenoisePass: Vk::RenderPass<
                           "RtrDenoise", Vk::ComputeWrite<Res_RtrHalf>, Vk::ComputeWrite<Res_RtgiDenoiseA>, Vk::ComputeWrite<Res_RtgiDenoiseB>,
                           Vk::ComputeRead<Res_RtrHistory>, Vk::ComputeRead<Res_Velocity>, Vk::ComputeRead<Res_Depth>, Vk::ComputeRead<Res_NormRough>> {
    RenderFrameContext&   impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
