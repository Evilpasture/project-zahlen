// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan/VulkanHeader.hpp"

#include "render/PassPayloads.hpp"
#include "render/RenderTargetTags.hpp"
#include "vulkan/graph/RenderPassDecl.hpp"

namespace ZHLN {
struct CullingResources;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct GBufferBasePass:
    Vk::RenderPass<
        "MainPass1",
        Vk::ColorWrite<Res_SceneColor>,
        Vk::ColorWrite<Res_Velocity>,
        Vk::ColorWrite<Res_NormRough>,
        Vk::ColorWrite<Res_BakedMask>,
        Vk::DepthStencilWrite<Res_Depth>> {
    using PushConstants = ObjectConstants;

    // The CPU-culled path records its draw range across worker secondaries, so
    // the graph must open the scope with
    // VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT.
    static constexpr bool uses_secondaries = true;

    CullingResources&   cull;
    RenderFrame&        frame;

    void operator()(Vk::RasterPassContextBase& pass) const noexcept;
};

} // namespace ZHLN::Passes
