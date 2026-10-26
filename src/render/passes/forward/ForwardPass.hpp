// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PassPayloads.hpp"
#include "RenderTargetTags.hpp"
#include "graph/RenderPassDecl.hpp"

namespace ZHLN {
struct ForwardResources;
struct RenderFrameContext;
} // namespace ZHLN

namespace ZHLN::Passes {

struct ForwardPass: Vk::RenderPass<"Forward", Vk::ColorWrite<Res_HdrSceneColor>, Vk::DepthStencilWrite<Res_Depth>, Vk::ShaderRead<Res_TransLighting>> {
    using PushConstants = ObjectConstants;

    const ForwardResources& forward;
    RenderFrameContext&     frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
