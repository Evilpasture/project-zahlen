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

struct TranslucentPrePass:
    VkD3D::RenderPass<
        "TransPrePass",
        VkD3D::ColorWrite<Res_TransNorm>,
        VkD3D::ColorWrite<Res_TransAnisotropy>,
        VkD3D::ColorWrite<Res_TransSheen>,
        VkD3D::DepthStencilWrite<Res_TransDepth>> {
    RenderFrame& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
