// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Filters the noisy half-resolution incoming-radiance signal before it is
// multiplied by G-buffer albedo. This preserves texture detail in the final
// scene while keeping the bilateral GI upsample edge-aware.
struct RtgiDenoisePass: Vk::RenderPass<
                            "RtgiDenoise", Vk::ComputeWrite<Res_RtgiHalf>, Vk::ComputeWrite<Res_RtgiDenoiseA>, Vk::ComputeWrite<Res_RtgiDenoiseB>,
                            Vk::ComputeRead<Res_RtgiHistory>, Vk::ComputeRead<Res_Velocity>, Vk::ComputeRead<Res_Depth>, Vk::ComputeRead<Res_NormRough>> {
    RenderFrameContext&   impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
