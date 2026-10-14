// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

struct TranslucentPrePass: Vk::RenderPass<"TransPrePass", Vk::ColorWrite<Res_TransNorm>, Vk::ColorWrite<Res_TransAnisotropy>, Vk::ColorWrite<Res_TransSheen>, Vk::DepthStencilWrite<Res_TransDepth>> {
    RenderFrameContext&   impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
