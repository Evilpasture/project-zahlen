// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderTargetTags.hpp"
#include "graph/RenderPassDecl.hpp"

namespace ZHLN {
struct RenderFrameContext;
} // namespace ZHLN

namespace ZHLN::Passes {

struct TranslucentPrePass:
    Vk::RenderPass<
        "TransPrePass",
        Vk::ColorWrite<Res_TransNorm>,
        Vk::ColorWrite<Res_TransAnisotropy>,
        Vk::ColorWrite<Res_TransSheen>,
        Vk::DepthStencilWrite<Res_TransDepth>> {
    RenderFrameContext& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
