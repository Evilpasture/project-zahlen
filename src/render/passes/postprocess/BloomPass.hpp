// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Kawase bloom: a bright-pass into a half-resolution threshold target, three
// downsamples, then three upsamples that add the coarser level back in. The
// whole chain is compute, so the pass owns seven targets' worth of usage
// declarations and the barriers between them are emitted inside the chain
// itself.
struct BloomPass: Vk::RenderPass<
                      "BloomKawase", Vk::ComputeReadGeneral<Res_HdrSceneColor>, Vk::ComputeRead<Res_Emissive>, Vk::ComputeWrite<Res_BloomThresh>,
                      Vk::ComputeWrite<Res_BloomDown1>, Vk::ComputeWrite<Res_BloomDown2>, Vk::ComputeWrite<Res_BloomDown3>, Vk::ComputeWrite<Res_BloomUp2>,
                      Vk::ComputeWrite<Res_BloomUp1>, Vk::ComputeWrite<Res_BloomFinal>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
