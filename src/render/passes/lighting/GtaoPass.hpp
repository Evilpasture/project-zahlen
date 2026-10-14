// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct GtaoPass: Vk::RenderPass<"GtaoAo", Vk::FragmentAndComputeRead<Res_Depth>, Vk::FragmentAndComputeRead<Res_NormRough>, Vk::ComputeWrite<Res_Ao>> {
    RenderFrameContext&                            impl;
    GeneratedGpu::ScenePassPushConstants          pc {};

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
