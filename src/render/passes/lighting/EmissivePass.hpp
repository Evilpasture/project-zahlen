// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "render/PassPayloads.hpp"
#include "render/RenderTargetTags.hpp"
#include "vulkan/graph/RenderPassDecl.hpp"

namespace ZHLN {
struct ForwardResources;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

// Compact deferred materials cannot store an emissive attachment. Their
// material emission is sampled a second time and additively rasterized into
// HDR LightingGI after GI composition, before reflection reads that target.
struct EmissivePass:
    Vk::RenderPass<"Emissive", Vk::ColorWriteRGB<Res_LightingGI>, Vk::DepthStencilWrite<Res_Depth>> {
    using PushConstants = ObjectConstants;

    const ForwardResources& forward;
    RenderFrame&            frame;

    void operator()(Vk::RasterPassContextBase& pass) const noexcept;
};

} // namespace ZHLN::Passes
