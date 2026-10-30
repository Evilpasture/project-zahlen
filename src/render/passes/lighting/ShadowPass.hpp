// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderTargetTags.hpp"
#include "graph/RenderPassDecl.hpp"

namespace ZHLN {
struct MeshParticlePipelines;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct ShadowPass: Vk::RenderPass<"MainShadow", Vk::DepthWrite<Res_ShadowMap>, Vk::DepthWrite<Res_ShadowAtlas>> {
    const MeshParticlePipelines& particles;
    RenderFrame&                 frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
