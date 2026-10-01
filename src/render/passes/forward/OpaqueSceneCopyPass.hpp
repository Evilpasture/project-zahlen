// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Seeds the transmission target with the opaque HDR scene color, then builds
// its mip chain for rough refraction in the forward pass. The graph enters in
// transfer-dst layout; GenerateMipmaps leaves every level shader-readable.
struct OpaqueSceneCopyPass: Vk::RenderPass<"OpaqueSceneCopy", Vk::TransferSrcRead<Res_HdrSceneColor>, Vk::TransferDstWriteThenShaderRead<Res_TransLighting>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
