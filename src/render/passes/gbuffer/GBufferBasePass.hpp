// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// The first GBuffer half: fills the seven color targets and the depth target
// from scratch, either by culling on the GPU into an indirect command buffer or
// by replaying the draw queue directly through secondary command buffers.
//
// Both paths open the render pass themselves -- the secondary-buffer path has
// to, since `VkCmdExecuteCommands` is only legal inside an active render pass
// that was begun with the secondary-content flag -- which is why this pass takes
// a bare `VkCommandBuffer` instead of an automatic render-pass context.
struct GBufferBasePass: Vk::RenderPass<
                            "MainPass1", Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_Velocity>, Vk::ColorWrite<Res_NormRough>,
                            Vk::ColorWrite<Res_Emissive>, Vk::ColorWrite<Res_Clearcoat>, Vk::ColorWrite<Res_Anisotropy>, Vk::ColorWrite<Res_Sheen>, Vk::DepthStencilWrite<Res_Depth>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
