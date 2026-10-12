// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Bilaterally upsamples the denoised half-resolution diffuse sample and writes
// a separate full-resolution target. When RTGI is disabled, this pass is a copy
// of direct lighting, so downstream reflection passes have one stable input.
struct RtgiCompositePass: Vk::RenderPass<
                              "RtgiComposite", Vk::ComputeRead<Res_SceneColor>, Vk::ComputeRead<Res_Depth>, Vk::ComputeRead<Res_NormRough>,
                              Vk::ComputeRead<Res_Anisotropy>, Vk::ComputeRead<Res_Clearcoat>, Vk::ComputeRead<Res_Sheen>,
                              Vk::ComputeRead<Res_Lighting>, Vk::ComputeRead<Res_RtgiHalf>, Vk::ComputeWrite<Res_LightingGI>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
