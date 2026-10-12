// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// One diffuse bounce sample per half-resolution pixel. It reads the direct-lit
// lighting target and emits noisy incoming radiance; a separate bilateral
// composite resolves it into LightingGI without making the trace feed itself.
struct RtgiHalfTracePass: Vk::RenderPass<
                              "RtgiHalfTrace", Vk::ComputeRead<Res_Depth>, Vk::ComputeRead<Res_NormRough>, Vk::ComputeRead<Res_Anisotropy>,
                              Vk::ComputeRead<Res_Lighting>, Vk::ComputeWrite<Res_RtgiHalf>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
