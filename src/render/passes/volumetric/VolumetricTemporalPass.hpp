// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Blends this frame's integrated volume against the reprojected history, which
// is what makes an under-sampled march usable: the noise is averaged away over
// frames instead of being resolved by brute force. `EndFrame` swaps the
// resolved volume into the history slot, so `Res_VoxelHist` and
// `Res_VoxelResolved` are two names for the same pair of images.
struct VolumetricTemporalPass:
    Vk::RenderPass<"VolumetricTemporal", Vk::ComputeReadGeneral<Res_VoxelInt>, Vk::ComputeReadGeneral<Res_VoxelHist>, Vk::ComputeWrite<Res_VoxelResolved>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
