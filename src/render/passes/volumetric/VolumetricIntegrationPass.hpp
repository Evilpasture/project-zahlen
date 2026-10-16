// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct VolumetricIntegrationPass: Vk::RenderPass<"VolumetricIntegrate", Vk::ComputeReadGeneral<Res_VoxelLight>, Vk::ComputeWrite<Res_VoxelInt>> {
    RenderFrameContext& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
