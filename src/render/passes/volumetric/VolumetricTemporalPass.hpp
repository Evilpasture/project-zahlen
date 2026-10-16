// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct VolumetricTemporalPass:
    Vk::RenderPass<"VolumetricTemporal", Vk::ComputeReadGeneral<Res_VoxelInt>, Vk::ComputeReadGeneral<Res_VoxelHist>, Vk::ComputeWrite<Res_VoxelResolved>> {
    RenderFrameContext& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
