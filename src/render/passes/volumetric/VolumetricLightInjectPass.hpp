// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Scatters clustered light into the media grid: for every voxel, the lights
// the cluster culling pass assigned to its froxel, each shadowed by the
// cascade map. It reads the media grid as a general-layout image because the
// inject pass left it writable.
struct VolumetricLightInjectPass:
    Vk::RenderPass<"VolumetricLightInject", Vk::ComputeReadGeneral<Res_VoxelMedia>, Vk::ComputeWrite<Res_VoxelLight>, Vk::ComputeRead<Res_ShadowMap>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
