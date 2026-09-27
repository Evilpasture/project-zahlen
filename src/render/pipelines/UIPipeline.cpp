// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "UIPipeline.hpp"
#include <Zahlen/Log.hpp>

namespace ZHLN::Pipelines {

namespace {

[[nodiscard]] auto ConfigureViewport(Vk::DynamicPass<0, false> pass, const ViewportRect& viewport) noexcept -> Vk::DynamicPass<0, false> {
    if (viewport.width > 0 && viewport.height > 0) {
        return Vk::DynamicPass<0, false>(std::move(pass).Viewport(
            static_cast<float>(viewport.x), static_cast<float>(viewport.y), static_cast<float>(viewport.width), static_cast<float>(viewport.height)
        ));
    }
    return pass;
}

}

auto UIPipeline::Execute(RenderContext::Impl& impl, const UIView& view, const UIDrawData& uiData) noexcept -> FrameOutcome<FrameSkipped> {
    if (uiData.Empty() || !view.target.Valid()) {
        return FrameSkipped {};
    }

    auto resolved = impl.destinations.Resolve(view.target);
    if (!resolved) {
        ZHLN::Log("[RenderUI] Attachment does not resolve to a render target ({}); UI skipped.", resolved.error().reason);
        return FrameSkipped {};
    }
    const DestinationRegistry::Record target = *resolved;

    const VkCommandBuffer cmd = impl.RecordingFor(target);
    if (cmd == VK_NULL_HANDLE) {
        ZHLN::Log("[RenderUI] Destination 0x{:016X} has no recording open this frame (was it acquired?); UI skipped.", target.handle.Raw());
        return FrameSkipped {};
    }

    const bool   firstTouch  = target.trackedLayout == Vk::AttachmentLayout::Undefined;
    const auto   sourceLayout = Vk::ToVkImageLayout(target.trackedLayout);
    if (sourceLayout != Vk::ToVkImageLayout(Vk::AttachmentLayout::ColorAttachment)) {
        const VkImageMemoryBarrier2 barrier = Vk::MakeImageBarrier({
            .image      = target.image.handle,
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

    const auto image = target.image.Assume<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>();
    const VkExtent2D extent = target.image.Extent2D();

    ConfigureViewport(Vk::DynamicPass(extent), view.viewport)
        .AddColor(
            image, firstTouch ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
            ZHLN::Color4 {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F}
        )
        .Execute(cmd, [&]() -> void {
            impl.BindHeapsAndPushFrame(cmd);

            Vk::CommandEncoder encoder(cmd);
            impl.uiRenderer.Record(encoder, extent.width, extent.height, view.frameIndex, uiData);
        });

    impl.destinations.NoteWritten(view.target, DestinationRegistry::Rendered::By::UI, Vk::AttachmentLayout::ColorAttachment);
    return std::nullopt;
}

}
