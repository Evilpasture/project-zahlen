// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct OpaqueSceneCopyPass: Vk::RenderPass<"OpaqueSceneCopy", Vk::TransferSrcRead<Res_HdrSceneColor>, Vk::TransferDstWriteThenShaderRead<Res_TransLighting>> {
    RenderFrameContext&   impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
