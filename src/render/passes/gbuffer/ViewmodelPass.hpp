// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderTargetTags.hpp"
#include "graph/RenderPassDecl.hpp"

namespace ZHLN {
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct ViewmodelPass:
    Vk::RenderPass<
        "Viewmodel",
        Vk::ColorWrite<Res_SceneColor>,
        Vk::ColorWrite<Res_Velocity>,
        Vk::ColorWrite<Res_NormRough>,
        Vk::ColorWrite<Res_BakedMask>,
        Vk::DepthStencilWrite<Res_Depth>> {
    RenderFrame& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
