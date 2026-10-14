// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct RtgiCompositePass: Vk::RenderPass<
                              "RtgiComposite", Vk::ComputeRead<Res_SceneColor>, Vk::ComputeRead<Res_Depth>, Vk::ComputeRead<Res_NormRough>,
                              Vk::ComputeRead<Res_Anisotropy>, Vk::ComputeRead<Res_Clearcoat>, Vk::ComputeRead<Res_Sheen>,
                              Vk::ComputeRead<Res_Lighting>, Vk::ComputeRead<Res_RtgiHalf>, Vk::ComputeWrite<Res_LightingGI>> {
    RenderFrameContext&   impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
