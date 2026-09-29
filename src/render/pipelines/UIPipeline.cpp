// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "UIPipeline.hpp"

namespace ZHLN::Pipelines {

namespace {

// Compile-time regression checks for the typed UI path: a verified image
// produces a format-aware pass, and only a matching builder-created pipeline
// can be bound to it (including color order and depth presence).
using LdrImage = Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_FORMAT_R8G8B8A8_UNORM>;
static_assert(std::same_as<decltype(std::declval<Vk::ImageSlice>().MatchFormat<VK_FORMAT_R8G8B8A8_UNORM,
                                                                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>()),
                           std::optional<LdrImage>>);
using LdrPass = decltype(Vk::DynamicPass(VkExtent2D {}).AddColor(std::declval<LdrImage>()));
static_assert(std::same_as<LdrPass, UIColorPass<VK_FORMAT_R8G8B8A8_UNORM>>);
static_assert(!std::is_constructible_v<LdrImage, VkImage, VkImageView, VkExtent3D, VkImageAspectFlags, const VkImageViewCreateInfo&>);
static_assert(!std::is_constructible_v<LdrImage, VkImage, VkImageView, VkExtent3D, VkImageAspectFlags, const VkImageViewCreateInfo*>);
static_assert(!std::is_constructible_v<LdrImage, Vk::ImageSlice>);
static_assert(!std::is_constructible_v<LdrPass, VkExtent2D>);
static_assert(!std::is_constructible_v<LdrPass, UIColorPass<VK_FORMAT_R8G8B8A8_SRGB>&&>);

template <VkFormat Target, VkFormat Pipeline>
concept CanBindUI = requires(const UIColorPass<Target>& pass, VkCommandBuffer cmd,
                             const Vk::TypedPipeline<1, false, Vk::AttachmentFormats<VK_FORMAT_UNDEFINED, Pipeline>>& pipeline) {
    pass.Bind(cmd, pipeline);
};
static_assert(CanBindUI<VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM>);
static_assert(!CanBindUI<VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB>);

template <typename Pass, typename Pipeline>
concept CanBindPass = requires(const Pass& pass, VkCommandBuffer cmd, const Pipeline& pipeline) { pass.Bind(cmd, pipeline); };
using DepthImage = Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_FORMAT_D32_SFLOAT_S8_UINT>;
using LdrDepthPass = decltype(std::declval<LdrPass>().AddDepth(std::declval<DepthImage>()));
using LdrDepthPipeline = Vk::TypedPipeline<1, true, Vk::AttachmentFormats<VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_R8G8B8A8_UNORM>>;
static_assert(CanBindPass<LdrDepthPass, LdrDepthPipeline>);
static_assert(!CanBindPass<LdrDepthPass, Vk::TypedPipeline<1, false, Vk::AttachmentFormats<VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8B8A8_UNORM>>>);
using MixedPass = decltype(std::declval<LdrPass>().AddColor(std::declval<Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_FORMAT_R8G8B8A8_SRGB>>()));
static_assert(!CanBindPass<MixedPass, Vk::TypedPipeline<2, false,
                         Vk::AttachmentFormats<VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM>>>);

using BuiltLdrPipeline = decltype(std::declval<Vk::PipelineBuilder<>>()
    .ColorFormats<VK_FORMAT_R8G8B8A8_UNORM>().NoDepth().Build(std::declval<VkDevice>()));
static_assert(std::same_as<typename BuiltLdrPipeline::value_type,
                           Vk::TypedPipeline<1, false, Vk::AttachmentFormats<VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8B8A8_UNORM>>>);
static_assert(!std::is_constructible_v<typename BuiltLdrPipeline::value_type, Vk::Pipeline&&>);
using RuntimePipeline = decltype(std::declval<Vk::PipelineBuilder<>>()
    .ColorFormats(std::declval<const std::array<VkFormat, 1>&>()).NoDepth().Build(std::declval<VkDevice>()));
static_assert(std::same_as<typename RuntimePipeline::value_type, Vk::TypedPipeline<1, false>>);

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
    if (uiData.Empty()) {
        return FrameSkipped {};
    }

    auto resolved = impl.ResolveTarget(view.target);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    auto& target = *resolved;
    if (!target.image.Valid()) {
        return std::unexpected(DestinationError::ExpiredFrameTarget);
    }
    if (!impl.uiRenderer.SupportsFormat(target.image.format)) {
        return std::unexpected(DestinationError::UnsupportedColorFormat);
    }

    const VkCommandBuffer cmd = target.recorder.Handle();
    impl.destinations.SetActive(target.window.id);

    const bool   firstTouch  = target.layout == Vk::AttachmentLayout::Undefined;
    const auto   sourceLayout = Vk::ToVkImageLayout(target.layout);
    if (sourceLayout != Vk::ToVkImageLayout(Vk::AttachmentLayout::ColorAttachment)) {
        const VkImageMemoryBarrier2 barrier = Vk::MakeImageBarrier({
            .image      = target.image.Handle(),
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

    const VkExtent2D extent = target.image.Extent2D();
    auto Draw = [&](const auto& image, auto&& record) -> FrameOutcome<FrameSkipped> {
        const auto pass = ConfigureViewport(Vk::DynamicPass(extent), view.viewport)
            .AddColor(
                image, firstTouch ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                ZHLN::Color4 {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F}
            );
        pass.Execute(cmd, [&]() {
            impl.BindHeapsAndPushFrame(cmd);
            Vk::CommandEncoder encoder(cmd);
            record(pass, encoder);
        });
        target.layout = Vk::AttachmentLayout::ColorAttachment;
        target.drawn = true;
        impl.warnedUnwrittenTarget = false;
        return std::nullopt;
    };

    auto DrawTyped = [&]<VkFormat Format>() -> FrameOutcome<FrameSkipped> {
        auto image = target.image.MatchFormat<Format, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>();
        if (!image) {
            return std::unexpected(DestinationError::UnsupportedColorFormat);
        }
        return Draw(*image, [&](const auto& pass, Vk::CommandEncoder& encoder) {
            impl.uiRenderer.Record<Format>(pass, encoder, extent.width, extent.height, view.frameIndex, uiData);
        });
    };

    // VkFormat is supplied by Vulkan, not a reflected engine enum. Switch only
    // over the formats whose pipelines were built with compile-time signatures.
    switch (target.image.format) {
        case VK_FORMAT_B8G8R8A8_SRGB:       return DrawTyped.template operator()<VK_FORMAT_B8G8R8A8_SRGB>();
        case VK_FORMAT_B8G8R8A8_UNORM:      return DrawTyped.template operator()<VK_FORMAT_B8G8R8A8_UNORM>();
        case VK_FORMAT_R8G8B8A8_SRGB:       return DrawTyped.template operator()<VK_FORMAT_R8G8B8A8_SRGB>();
        case VK_FORMAT_R8G8B8A8_UNORM:      return DrawTyped.template operator()<VK_FORMAT_R8G8B8A8_UNORM>();
        case VK_FORMAT_R16G16B16A16_SFLOAT: return DrawTyped.template operator()<VK_FORMAT_R16G16B16A16_SFLOAT>();
        default:
            // Preserve presentation on surfaces with an uncommon VkFormat;
            // SupportsFormat admitted only the pipeline built for this exact
            // format, so the untyped fallback cannot bind a different one.
            return Draw(target.image.Assume<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(),
                        [&](const auto&, Vk::CommandEncoder& encoder) {
                            impl.uiRenderer.RecordFallback(encoder, extent.width, extent.height, view.frameIndex, target.image.format, uiData);
                        });
    }
}

}
