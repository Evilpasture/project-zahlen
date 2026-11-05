// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderTargetTags.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>

namespace ZHLN {
struct DecalResources;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

// A decal repaints albedo and normal/roughness over a surface it does not own,
// so it deliberately leaves Res_BakedMask alone: the pixels underneath keep the
// ambient ownership the G-buffer pass gave them, which is what a decal on a
// lightmapped wall should inherit. Its pipeline is also a two-attachment pass of
// its own (BuildDecalPipeline), not a variant of the compact G-buffer.
struct DecalPass: VkD3D::RenderPass<"DecalPass", VkD3D::ShaderRead<Res_Depth>, VkD3D::ColorWrite<Res_SceneColor>, VkD3D::ColorWrite<Res_NormRough>> {
    const DecalResources& decals;
    RenderFrame&          frame;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

} // namespace ZHLN::Passes
