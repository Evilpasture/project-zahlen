// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Half-resolution ground-truth ambient occlusion, and the diffuse GI estimate
// the same dispatch produces. It runs only when the selected GI mode is one of
// the two that need an AO term, and it is a compute pass: it reads depth and
// normal/roughness and writes the AO target directly.
struct GtaoPass: Vk::RenderPass<"GtaoAo", Vk::ShaderRead<Res_Depth>, Vk::ShaderRead<Res_NormRough>, Vk::ComputeWrite<Res_Ao>> {
    RenderContext::Impl&                          impl;
    GeneratedGpu::ScenePassPushConstants          pc {};

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
