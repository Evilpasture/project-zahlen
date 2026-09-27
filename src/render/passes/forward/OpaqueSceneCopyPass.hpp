// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Seeds the translucent lighting target with the opaque HDR scene color. The
// forward pass that follows draws translucents over it, so a surface that is
// not covered by any translucent draw still shows the opaque scene rather than
// a black hole. It is a plain image copy, hence the transfer usages.
struct OpaqueSceneCopyPass: Vk::RenderPass<"OpaqueSceneCopy", Vk::TransferSrcRead<Res_HdrSceneColor>, Vk::TransferDstWrite<Res_TransLighting>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
