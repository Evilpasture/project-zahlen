// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Fills the voxel grid with participating media: each fog volume's density,
// height falloff, scattering and emission, modulated by the tiling 3D noise
// `VolumetricFogSystem` uploaded at startup.
struct VolumetricFogInjectPass: Vk::RenderPass<"VolumetricFogInject", Vk::ComputeWrite<Res_VoxelMedia>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
