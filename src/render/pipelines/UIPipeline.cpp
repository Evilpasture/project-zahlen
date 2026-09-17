// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/pipelines/UIPipeline.cpp

#include "UIPipeline.hpp"
#include <Zahlen/Log.hpp>

namespace ZHLN::Pipelines {

namespace {

/// Applies the caller's viewport rectangle when it named one. Taking and
/// returning the pass by value keeps the builder's `&&`-qualified chain on a
/// live object instead of on an expiring temporary.
[[nodiscard]] auto ConfigureViewport(Vk::DynamicPass<0, false> pass, const ViewportRect& viewport) noexcept -> Vk::DynamicPass<0, false> {
    if (viewport.width > 0 && viewport.height > 0) {
        return Vk::DynamicPass<0, false>(std::move(pass).Viewport(
            static_cast<float>(viewport.x), static_cast<float>(viewport.y), static_cast<float>(viewport.width), static_cast<float>(viewport.height)
        ));
    }
    return pass;
}

} // namespace

void UIPipeline::Execute(RenderContext::Impl& impl, VkCommandBuffer cmd, const UIView& view, const UIDrawData& uiData) noexcept {
    if (cmd == VK_NULL_HANDLE || uiData.Empty() || !view.target.Valid()) {
        return;
    }

    // 1. Subresource -> concrete image. The record is copied on purpose:
    //    registering a render target may grow the registry, so nothing may hold
    //    the record's address across frames.
    auto resolved = impl.ResolveAttachment(view.target);
    if (!resolved.has_value()) {
        ZHLN::Log("[RenderUI] Attachment does not resolve to a render target; UI skipped.");
        return;
    }
    const RenderContext::Impl::RenderTargetRecord target = *resolved;

    // 2. Move the target into the layout this pass renders in. A target
    //    acquired this frame starts UNDEFINED, so its contents are undefined
    //    and the pass clears rather than loading whatever was there before;
    //    anything already written earlier this frame is preserved.
    const bool   firstTouch = target.trackedLayout == AttachmentLayout::Undefined;
    const auto   sourceLayout = ToVkImageLayout(target.trackedLayout);
    if (sourceLayout != ToVkImageLayout(AttachmentLayout::ColorAttachment)) {
        const VkImageMemoryBarrier2 barrier = Vk::MakeImageBarrier({
            .image      = target.image,
            .src_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dst_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
            .src_layout = sourceLayout,
            .dst_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .src_stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dst_stage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
            .base_mip   = 0,
            .mip_count  = VK_REMAINING_MIP_LEVELS,
        });
        Vk::PipelineBarrier(cmd, std::span<const VkBufferMemoryBarrier2> {}, std::span<const VkImageMemoryBarrier2> {&barrier, 1});
    }

    // 3. One dynamic pass over the destination: no depth, no scene state.
    const Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> image {
        .handle = target.image,
        .view   = target.view,
        .extent = target.extent,
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        .format = target.format,
    };
    const VkExtent2D extent {.width = target.extent.width, .height = target.extent.height};

    ConfigureViewport(Vk::DynamicPass(extent), view.viewport)
        .AddColor(
            image, firstTouch ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
            ZHLN::Color4 {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F}
        )
        .Execute(cmd, [&]() -> void {
            // The UI pipeline's mappings address the sampler heap and the texture
            // array, so the heaps and the per-frame address block must be current.
            impl.BindHeapsAndPushFrame(cmd);

            Vk::CommandEncoder encoder(cmd, &impl.ctx);
            impl.uiRenderer.Record(encoder, extent.width, extent.height, view.frameIndex, uiData);
        });

    impl.NoteAttachmentWritten(view.target, AttachmentLayout::ColorAttachment);
}

} // namespace ZHLN::Pipelines
