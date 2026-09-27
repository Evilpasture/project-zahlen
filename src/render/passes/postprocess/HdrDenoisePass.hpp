// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// The A-Trous wavelet ladder that cleans up ray-traced shadows and
// reflections. Each iteration widens its step and uses depth and normal to
// keep the filter from bleeding across silhouettes; the ladder always ends by
// writing back into the HDR scene color, so the number of ping-pong targets
// depends on how many iterations were asked for.
struct HdrDenoisePass: Vk::RenderPass<
                           "HdrDenoise", Vk::ComputeWrite<Res_HdrSceneColor>, Vk::ComputeWrite<Res_DenoiseA>, Vk::ComputeWrite<Res_DenoiseB>,
                           Vk::ShaderRead<Res_Depth>, Vk::ShaderRead<Res_NormRough>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
