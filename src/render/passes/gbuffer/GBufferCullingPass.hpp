// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan/VulkanHeader.hpp"

#include "vulkan/graph/RenderPassDecl.hpp"

namespace ZHLN {
struct CullingResources;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

// A pass of its own, not the head of the G-buffer pass: none of this may be
// recorded inside vkCmdBeginRendering.
//
// The scope the G-buffer passes open used to swallow the counter clear, its
// barriers and the culling dispatch, and every one of those is illegal there:
// vkCmdFillBuffer must be outside a render pass instance
// (VUID-vkCmdFillBuffer-renderpass), so must vkCmdDispatch without the per-tile
// execution model (VUID-vkCmdDispatch-None-10672), and a barrier may not carry a
// buffer memory barrier inside one unless dynamicRenderingLocalRead is enabled
// (VUID-vkCmdPipelineBarrier2-None-09553/09554). This renderer enables neither
// that feature nor VK_EXT_shader_tile_image, so the commands belong here, ahead
// of the scope, which is where the pre-graph renderer recorded them.
//
// No image usages are declared on purpose. The HiZ map read below is last
// frame's for the first pass and this frame's for the second -- unchanged from
// before -- and the buffers are synced by the explicit calls in the body rather
// than by the graph, because the graph only tracks images.
struct GBufferCullingPass: Vk::RenderPass<"GBufferCull"> {
    CullingResources& cull;
    RenderFrame&      frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

// The second-pass variant, after this frame's HiZ exists: culls what the first
// pass could not decide into the second indirect buffer.
struct GBufferResolveCullingPass: Vk::RenderPass<"GBufferCull2"> {
    CullingResources& cull;
    RenderFrame&      frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

// Which path a frame takes has to be the same answer in four places: the two
// culling passes that must record, and the two G-buffer passes that must match
// them and report VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT
// accordingly. It used to be the same expression written twice; it is now one
// function so the passes cannot disagree about what the frame is doing.
[[nodiscard]] bool UsesGpuCulling(const CullingResources& cull, RenderFrame& frame, bool secondPass, uint32_t drawCount) noexcept;

} // namespace ZHLN::Passes
