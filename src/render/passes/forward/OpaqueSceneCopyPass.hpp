// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderTargetTags.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>

namespace ZHLN {
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct OpaqueSceneCopyPass: VkD3D::RenderPass<"OpaqueSceneCopy", VkD3D::TransferSrcRead<Res_HdrSceneColor>, VkD3D::TransferDstWriteThenShaderRead<Res_TransLighting>> {
    RenderFrame& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
