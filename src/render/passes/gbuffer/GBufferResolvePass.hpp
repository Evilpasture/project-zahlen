// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// The second GBuffer half: everything the base pass had to defer. Draws that
// survived occlusion by the depth it wrote are replayed against the same
// targets with load-instead-of-clear, and the stencil CSG and mesh-particle
// work that depends on a complete depth buffer lands here.
//
// It reads the HiZ pyramid the base pass produced, which is the one thing that
// orders these two passes: without it the graph would be free to run them in
// either order, or to fork them.
struct GBufferResolvePass: Vk::RenderPass<
                               "MainPass2", Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_Velocity>, Vk::ColorWrite<Res_NormRough>,
                               Vk::ColorWrite<Res_Emissive>, Vk::ColorWrite<Res_Clearcoat>, Vk::DepthStencilWrite<Res_Depth>, Vk::ComputeRead<Res_HiZ>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
