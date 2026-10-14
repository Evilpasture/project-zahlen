// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Full-resolution directional sun shadow ray trace into a dedicated R16F mask
// so it can be temporally and spatially denoised before ClusteredLightingPass
// shades the G-buffer.
struct RtShadowPass: Vk::RenderPass<
                         "RtShadow", Vk::FragmentAndComputeRead<Res_Depth>, Vk::FragmentAndComputeRead<Res_NormRough>, Vk::ComputeWrite<Res_RtShadowMask>> {
    RenderFrameContext&   impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
