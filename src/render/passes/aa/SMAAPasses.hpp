// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderTargetTags.hpp"
#include "graph/RenderPassDecl.hpp"

namespace ZHLN {
struct AntiAliasingResources;
struct RenderFrameContext;
} // namespace ZHLN

namespace ZHLN::Passes {

struct SmaaEdgePass: Vk::RenderPass<"SmaaEdge", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ColorWrite<Res_SmaaEdge>> {
    AntiAliasingResources& aa;
    RenderFrameContext&    frame;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

struct SmaaWeightPass: Vk::RenderPass<"SmaaWeight", Vk::ShaderRead<Res_SmaaEdge>, Vk::ColorWrite<Res_SmaaWeight>> {
    AntiAliasingResources& aa;
    RenderFrameContext&    frame;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

struct SmaaBlendPass: Vk::RenderPass<"SmaaBlend", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ShaderRead<Res_SmaaWeight>, Vk::ColorWrite<Res_AccumCurrent>> {
    AntiAliasingResources& aa;
    RenderFrameContext&    frame;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

} // namespace ZHLN::Passes
