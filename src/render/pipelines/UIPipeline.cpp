// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "UIPipeline.hpp"
#include <span>

namespace ZHLN::Pipelines {

namespace {

using LdrImage = Vk::TypedImage<Vk::ImageLayout::ColorAttachmentOptimal, Vk::Format::R8G8B8A8Unorm>;
static_assert(std::same_as<decltype(std::declval<Vk::ImageSlice>().MatchFormat<Vk::Format::R8G8B8A8Unorm,
                                                                                 Vk::ImageLayout::ColorAttachmentOptimal>()),
                           std::optional<LdrImage>>);
using LdrPass = decltype(Vk::DynamicPass(VkExtent2D {}).AddColor(std::declval<LdrImage>()));
static_assert(std::same_as<LdrPass, UIColorPass<Vk::Format::R8G8B8A8Unorm>>);
static_assert(!std::is_constructible_v<LdrImage, VkImage, VkImageView, VkExtent3D, Vk::ImageAspect, const Vk::ImageView&>);
static_assert(!std::is_constructible_v<LdrImage, VkImage, VkImageView, VkExtent3D, Vk::ImageAspect, const Vk::ImageView*>);
static_assert(!std::is_constructible_v<LdrImage, Vk::ImageSlice>);
static_assert(!std::is_constructible_v<LdrPass, VkExtent2D>);
static_assert(!std::is_constructible_v<LdrPass, UIColorPass<Vk::Format::R8G8B8A8Srgb>&&>);

template <Vk::Format Target, Vk::Format Pipeline>
concept CanBindUI = requires(const UIColorPass<Target>& pass, VkCommandBuffer cmd,
                             const Vk::TypedPipeline<1, false, Vk::AttachmentFormats<Vk::Format::Undefined, Pipeline>>& pipeline) {
    pass.Bind(cmd, pipeline);
};
static_assert(CanBindUI<Vk::Format::R8G8B8A8Unorm, Vk::Format::R8G8B8A8Unorm>);
static_assert(!CanBindUI<Vk::Format::R8G8B8A8Unorm, Vk::Format::R8G8B8A8Srgb>);

template <typename Pass, typename Pipeline>
concept CanBindPass = requires(const Pass& pass, VkCommandBuffer cmd, const Pipeline& pipeline) { pass.Bind(cmd, pipeline); };
using DepthImage = Vk::TypedImage<Vk::ImageLayout::DepthStencilAttachmentOptimal, Vk::Format::D32SfloatS8Uint>;
using LdrDepthPass = decltype(std::declval<LdrPass>().AddDepth(std::declval<DepthImage>()));
using LdrDepthPipeline = Vk::TypedPipeline<1, true, Vk::AttachmentFormats<Vk::Format::D32SfloatS8Uint, Vk::Format::R8G8B8A8Unorm>>;
static_assert(CanBindPass<LdrDepthPass, LdrDepthPipeline>);
static_assert(!CanBindPass<LdrDepthPass, Vk::TypedPipeline<1, false, Vk::AttachmentFormats<Vk::Format::Undefined, Vk::Format::R8G8B8A8Unorm>>>);
using MixedPass = decltype(std::declval<LdrPass>().AddColor(std::declval<Vk::TypedImage<Vk::ImageLayout::ColorAttachmentOptimal, Vk::Format::R8G8B8A8Srgb>>()));
static_assert(!CanBindPass<MixedPass, Vk::TypedPipeline<2, false,
                         Vk::AttachmentFormats<Vk::Format::Undefined, Vk::Format::R8G8B8A8Srgb, Vk::Format::R8G8B8A8Unorm>>>);

using BuiltLdrPipeline = decltype(std::declval<Vk::PipelineBuilder<>>()
    .ColorFormats<Vk::Format::R8G8B8A8Unorm>().NoDepth().Build(std::declval<VkDevice>()));
static_assert(std::same_as<typename BuiltLdrPipeline::value_type,
                           Vk::TypedPipeline<1, false, Vk::AttachmentFormats<Vk::Format::Undefined, Vk::Format::R8G8B8A8Unorm>>>);
static_assert(!std::is_constructible_v<typename BuiltLdrPipeline::value_type, Vk::Pipeline&&>);
using RuntimePipeline = decltype(std::declval<Vk::PipelineBuilder<>>()
    .ColorFormats(std::declval<std::span<const Vk::Format, 1>>()).NoDepth().Build(std::declval<VkDevice>()));
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

auto UIPipeline::Execute(RenderContext::Impl& impl, const UIView& view, UIDrawData uiData) noexcept -> FrameOutcome<FrameSkipped> {
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

    const bool   firstTouch   = target.layout == Vk::AttachmentLayout::Undefined;
    const auto   sourceLayout = Vk::ToVkImageLayout(target.layout);
    if (sourceLayout != Vk::ToVkImageLayout(Vk::AttachmentLayout::ColorAttachment)) {
        target.image.Transition(
            cmd,
            sourceLayout,
            Vk::ImageLayout::ColorAttachmentOptimal,
            Vk::PipelineStage::AllCommands,
            Vk::Access::ColorWrite | Vk::Access::ShaderWrite | Vk::Access::TransferWrite,
            Vk::PipelineStage::ColorAttachment,
            Vk::Access::ColorWrite | Vk::Access::ColorRead
        );
    }

    const VkExtent2D extent = target.image.Extent2D();
    auto Draw = [&](const auto& image, auto&& record) -> FrameOutcome<FrameSkipped> {
        const auto pass = ConfigureViewport(Vk::DynamicPass(extent), view.viewport)
            .AddColor(image, firstTouch ? Vk::ColorOp::Clear(ZHLN::Color4 {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F}) : Vk::ColorOp::Load());
        pass.Execute(cmd, [&]() {
            Vk::CommandEncoder encoder(cmd);
            record(pass, encoder);
        });
        target.layout = Vk::AttachmentLayout::ColorAttachment;
        target.drawn = true;
        impl.diagnostics.warnedUnwrittenTarget = false;
        return std::nullopt;
    };

    auto DrawTyped = [&]<Vk::Format Format>() -> FrameOutcome<FrameSkipped> {
        auto image = target.image.MatchFormat<Format, Vk::ImageLayout::ColorAttachmentOptimal>();
        if (!image) {
            return std::unexpected(DestinationError::UnsupportedColorFormat);
        }
        return Draw(*image, [&](const auto& pass, Vk::CommandEncoder& encoder) {
            impl.uiRenderer.Record<Format>(pass, encoder, extent.width, extent.height, view.frameIndex, uiData);
        });
    };

    switch (target.image.format) {
        case Vk::Format::B8G8R8A8Srgb:       return DrawTyped.template operator()<Vk::Format::B8G8R8A8Srgb>();
        case Vk::Format::B8G8R8A8Unorm:      return DrawTyped.template operator()<Vk::Format::B8G8R8A8Unorm>();
        case Vk::Format::R8G8B8A8Srgb:       return DrawTyped.template operator()<Vk::Format::R8G8B8A8Srgb>();
        case Vk::Format::R8G8B8A8Unorm:      return DrawTyped.template operator()<Vk::Format::R8G8B8A8Unorm>();
        case Vk::Format::R16G16B16A16Sfloat: return DrawTyped.template operator()<Vk::Format::R16G16B16A16Sfloat>();
        default:

            return Draw(target.image.Assume<Vk::ImageLayout::ColorAttachmentOptimal>(),
                        [&](const auto&, Vk::CommandEncoder& encoder) {
                            impl.uiRenderer.RecordFallback(encoder, extent.width, extent.height, view.frameIndex, target.image.format, uiData);
                        });
    }
}

}
