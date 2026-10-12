// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Convolves Res_DdgiRays into the octahedral irradiance (Res_DdgiIrradiance)
// and Chebyshev distance moment (Res_DdgiDistance) probe atlases with temporal
// hysteresis and 1-texel octahedral border wrapping.
struct DdgiUpdateAtlasPass: Vk::RenderPass<
                                "DdgiUpdateAtlas", Vk::ComputeRead<Res_DdgiRays>, Vk::ComputeWrite<Res_DdgiIrradiance>, Vk::ComputeWrite<Res_DdgiDistance>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
