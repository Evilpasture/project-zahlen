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

    // The CPU-culled path records its draw range across worker secondaries, so the
    // graph must open the scope with VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT.
    // The GPU-culled path must NOT carry it: that path draws indirectly inside the
    // same scope, and the flag without VK_RENDERING_CONTENTS_INLINE_BIT_KHR is
    // VUID-vkCmdDrawIndirect-flags-10582 -- the inline bit exists only with
    // dynamicRenderingLocalRead, which this renderer never enables. Which path a
    // frame takes is known at record time, so this is answered per frame instead
    // of being declared as a constant.
    [[nodiscard]] bool UsesSecondaries() const noexcept;

    CullingResources&   cull;
    RenderFrame&        frame;

    void operator()(Vk::RasterPassContextBase& pass) const noexcept;
};

} // namespace ZHLN::Passes
