// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Traces one reflection ray per half-resolution pixel against the TLAS. The
// result is noisy and half-sized on purpose: the reflection pass up-samples
// it and the A-Trous ladder denoises what survives, which is far cheaper than
// tracing at full rate.
struct RtrHalfTracePass: Vk::RenderPass<
                             "RtrHalfTrace", Vk::ShaderRead<Res_Depth>, Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Anisotropy>, Vk::ShaderRead<Res_Lighting>,
                             Vk::ComputeWrite<Res_RtrHalf>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
