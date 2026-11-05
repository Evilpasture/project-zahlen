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

struct ForwardPass: VkD3D::RenderPass<"Forward", VkD3D::ColorWrite<Res_HdrSceneColor>, VkD3D::DepthStencilWrite<Res_Depth>, VkD3D::ShaderRead<Res_TransLighting>> {
    using PushConstants = ObjectConstants;

    const ForwardResources& forward;
    RenderFrame&            frame;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

} // namespace ZHLN::Passes
