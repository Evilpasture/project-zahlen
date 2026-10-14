// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Temporal reprojection and 5x5 edge-stopping A-Trous wavelet denoiser for the
// ray-traced directional shadow mask before deferred lighting reads it.
struct ShadowDenoisePass: Vk::RenderPass<
                              "ShadowDenoise", Vk::ComputeWrite<Res_RtShadowMask>, Vk::ComputeWrite<Res_ShadowDenoiseA>, Vk::ComputeWrite<Res_ShadowDenoiseB>,
                              Vk::ComputeRead<Res_RtShadowHistory>, Vk::ComputeRead<Res_Velocity>, Vk::ComputeRead<Res_Depth>, Vk::ComputeRead<Res_NormRough>> {
    RenderFrameContext&   impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
