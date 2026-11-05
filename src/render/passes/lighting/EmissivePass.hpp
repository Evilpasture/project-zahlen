// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PassPayloads.hpp"
#include "RenderTargetTags.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>

namespace ZHLN {
struct ForwardResources;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

// Compact deferred materials cannot store an emissive attachment. Their
// material emission is sampled a second time and additively rasterized into
// HDR LightingGI after GI composition, before reflection reads that target.
struct EmissivePass:
    VkD3D::RenderPass<"Emissive", VkD3D::ColorWriteRGB<Res_LightingGI>, VkD3D::DepthStencilWrite<Res_Depth>> {
    using PushConstants = ObjectConstants;

    const ForwardResources& forward;
    RenderFrame&            frame;

    void operator()(Vk::RasterPassContextBase& pass) const noexcept;
};

} // namespace ZHLN::Passes
