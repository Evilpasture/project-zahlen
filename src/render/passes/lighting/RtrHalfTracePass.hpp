// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct RtrHalfTracePass: Vk::RenderPass<
                             "RtrHalfTrace", Vk::ComputeRead<Res_Depth>, Vk::ComputeRead<Res_NormRough>, Vk::ComputeRead<Res_Anisotropy>, Vk::ComputeRead<Res_Lighting>,
                             Vk::ComputeRead<Res_DdgiIrradiance>, Vk::ComputeRead<Res_DdgiDistance>, Vk::ComputeWrite<Res_RtrHalf>> {
    RenderFrameContext&   impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
