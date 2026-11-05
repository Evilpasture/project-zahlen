// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PassPayloads.hpp"
#include "RenderTargetTags.hpp"
#include "graph/RenderPassDecl.hpp"

namespace ZHLN {
struct CullingResources;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct GBufferBasePass:
    Vk::RenderPass<
        "MainPass1",
        Vk::ColorWrite<Res_SceneColor>,
        Vk::ColorWrite<Res_Velocity>,
        Vk::ColorWrite<Res_NormRough>,
        Vk::ColorWrite<Res_BakedMask>,
        Vk::DepthStencilWrite<Res_Depth>> {
    using PushConstants = ObjectConstants;

    CullingResources&   cull;
    RenderFrame&        frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
