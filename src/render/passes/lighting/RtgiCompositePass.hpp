// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderTargetTags.hpp"
#include "graph/RenderPassDecl.hpp"

namespace ZHLN {
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct RtgiCompositePass:
    Vk::RenderPass<
        "RtgiComposite",
        Vk::ComputeRead<Res_SceneColor>,
        Vk::ComputeRead<Res_Depth>,
        Vk::ComputeRead<Res_NormRough>,
        Vk::ComputeRead<Res_Lighting>,
        Vk::ComputeRead<Res_LightingAo>,
        Vk::ComputeRead<Res_RcgiProbes>,
        Vk::ComputeRead<Res_RcgiCascadeA>,
        Vk::ComputeWrite<Res_LightingGI>> {
    RenderFrame& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
