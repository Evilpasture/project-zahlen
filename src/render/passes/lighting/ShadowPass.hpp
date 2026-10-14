// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct ShadowPass: Vk::RenderPass<"MainShadow", Vk::DepthWrite<Res_ShadowMap>, Vk::DepthWrite<Res_ShadowAtlas>> {
    const MeshParticlePipelines& particles;
    RenderFrameContext&          frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
