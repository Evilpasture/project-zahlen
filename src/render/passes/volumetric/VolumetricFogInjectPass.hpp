// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct VolumetricFogInjectPass: Vk::RenderPass<"VolumetricFogInject", Vk::ComputeWrite<Res_VoxelMedia>> {
    RenderFrameContext& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
