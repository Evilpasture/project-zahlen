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

struct ForwardPass: Vk::RenderPass<"Forward", Vk::ColorWrite<Res_HdrSceneColor>, Vk::DepthStencilWrite<Res_Depth>, Vk::ShaderRead<Res_TransLighting>> {
    using PushConstants = ObjectConstants;

    const ForwardResources& forward;
    RenderFrame&            frame;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept;
};

} // namespace ZHLN::Passes
