// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderTargetTags.hpp"
#include "graph/RenderPassDecl.hpp"

namespace ZHLN {
struct DecalResources;
struct RenderFrameContext;
} // namespace ZHLN

namespace ZHLN::Passes {

// A decal repaints albedo and normal/roughness over a surface it does not own,
// so it deliberately leaves Res_BakedMask alone: the pixels underneath keep the
// ambient ownership the G-buffer pass gave them, which is what a decal on a
// lightmapped wall should inherit. Its pipeline is also a two-attachment pass of
// its own (BuildDecalPipeline), not a variant of the compact G-buffer.
struct DecalPass: Vk::RenderPass<"DecalPass", Vk::ShaderRead<Res_Depth>, Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_NormRough>> {
    const DecalResources& decals;
    RenderFrameContext&   frame;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

} // namespace ZHLN::Passes
