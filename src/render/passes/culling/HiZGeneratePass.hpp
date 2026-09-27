// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Builds the hierarchical depth pyramid the GPU culling shader rejects
// instances against: mip 0 reduces the scene depth buffer, and every later mip
// reduces the one before it, so the whole ladder is `kMaxGeneratedHiZMips`
// dispatches separated by write-to-read barriers.
struct HiZGeneratePass: Vk::RenderPass<"HiZGenerate", Vk::ShaderRead<Res_Depth>, Vk::ComputeWrite<Res_HiZ>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
