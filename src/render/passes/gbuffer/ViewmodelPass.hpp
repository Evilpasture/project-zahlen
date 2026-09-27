// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// First-person geometry, drawn last over the resolved GBuffer with its own
// narrow projection so a held object does not clip into the world. It is the
// same attachment set as the GBuffer passes; only the matrices differ.
struct ViewmodelPass: Vk::RenderPass<
                          "Viewmodel", Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_Velocity>, Vk::ColorWrite<Res_NormRough>,
                          Vk::ColorWrite<Res_Emissive>, Vk::ColorWrite<Res_Clearcoat>, Vk::DepthStencilWrite<Res_Depth>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
