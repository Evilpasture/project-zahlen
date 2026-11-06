// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "render/RenderTargetTags.hpp"
#include "vulkan/graph/RenderPassDecl.hpp"

namespace ZHLN {
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct TranslucentPrePass:
    Vk::RenderPass<
        "TransPrePass",
        Vk::ColorWrite<Res_TransNorm>,
        Vk::ColorWrite<Res_TransAnisotropy>,
        Vk::ColorWrite<Res_TransSheen>,
        Vk::DepthStencilWrite<Res_TransDepth>> {
    RenderFrame& frame;

    void operator()(Vk::RasterPassContextBase& pass) const noexcept;
};

} // namespace ZHLN::Passes
