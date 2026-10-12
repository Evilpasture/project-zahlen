// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Casts stratified spherical Fibonacci rays from each world-space DDGI probe
// into the TLAS and evaluates direct sun + previous-frame multi-bounce DDGI
// irradiance into Res_DdgiRays.
struct DdgiRayTracePass: Vk::RenderPass<
                             "DdgiRayTrace", Vk::ComputeRead<Res_DdgiIrradiance>, Vk::ComputeRead<Res_DdgiDistance>, Vk::ComputeWrite<Res_DdgiRays>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
