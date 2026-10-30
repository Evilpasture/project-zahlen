// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <optional>
#include <type_traits>

#include "ImageView.hpp"

namespace ZHLN::Vk {

template <VkImageLayout Layout, VkFormat Format>
struct TypedImage;

struct ImageSlice {
    VkImage            image = VK_NULL_HANDLE;
    VkImageView        attachmentView = VK_NULL_HANDLE;
    VkExtent3D         extent {};
    VkFormat           format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t           baseMip = 0;
    uint32_t           mipCount = 1;
    uint32_t           baseLayer = 0;
    uint32_t           layerCount = 1;
    VkImageViewType    viewType = VK_IMAGE_VIEW_TYPE_2D;

    constexpr ImageSlice() noexcept = default;

    constexpr ImageSlice(
        VkImage img, VkImageView attachment, VkExtent2D ext, VkFormat fmt, VkImageAspectFlags asp = VK_IMAGE_ASPECT_COLOR_BIT
    ) noexcept:
        image(img), attachmentView(attachment), extent({ext.width, ext.height, 1}), format(fmt), aspect(asp) {
    }
    constexpr ImageSlice(
        VkImage img, VkImageView attachment, VkExtent3D ext, VkFormat fmt, VkImageAspectFlags asp = VK_IMAGE_ASPECT_COLOR_BIT
    ) noexcept:
        image(img), attachmentView(attachment), extent(ext), format(fmt), aspect(asp) {
    }

    constexpr explicit ImageSlice(ImageView view, VkExtent3D ext) noexcept: image(view.Handle()), extent(ext), format(view.Format()), aspect(view.Aspect()) {
        const VkImageSubresourceRange range = view.SubresourceRange();
        baseMip                              = range.baseMipLevel;
        mipCount                             = range.levelCount;
        baseLayer                            = range.baseArrayLayer;
        layerCount                           = range.layerCount;
        viewType                             = view.ViewType();
    }

    ImageSlice(
        VkImage img, const AttachmentView& owner, VkExtent3D ext, VkFormat fmt, VkImageAspectFlags asp = VK_IMAGE_ASPECT_NONE
    ) noexcept:
        image(img), attachmentView(owner.Get()), extent(ext), format(fmt), aspect(asp != VK_IMAGE_ASPECT_NONE ? asp : owner.AspectFlags()) {
        const VkImageSubresourceRange range = owner.View().SubresourceRange();
        baseMip                              = range.baseMipLevel;
        mipCount                             = range.levelCount;
        baseLayer                            = range.baseArrayLayer;
        layerCount                           = range.layerCount;
        viewType                             = owner.View().ViewType();
    }
    ImageSlice(
        VkImage img, const AttachmentView& owner, VkExtent2D ext, VkFormat fmt, VkImageAspectFlags asp = VK_IMAGE_ASPECT_NONE
    ) noexcept:
        ImageSlice(img, owner, VkExtent3D {ext.width, ext.height, 1}, fmt, asp) {
    }

    [[nodiscard]] constexpr auto View() const noexcept -> ImageView {
        return ImageView {
            image,
            format,
            aspect,
            static_cast<uint8_t>(baseMip),
            mipCount == VK_REMAINING_MIP_LEVELS ? ImageView::kRemainingMips : static_cast<uint8_t>(mipCount),
            static_cast<uint16_t>(baseLayer),
            layerCount == VK_REMAINING_ARRAY_LAYERS ? ImageView::kRemainingLayers : static_cast<uint16_t>(layerCount),
            viewType,
        };
    }

    template <VkImageLayout OldLayout, VkImageLayout NewLayout>
    void Transition(VkCommandBuffer cmd) const noexcept;

    template <VkImageLayout TargetLayout>
    void DiscardTo(VkCommandBuffer cmd) const noexcept;

    void Transition(VkCommandBuffer cmd, VkImageLayout oldLayout, VkImageLayout newLayout) const noexcept;

    void Transition(
        VkCommandBuffer       cmd,
        VkImageLayout         oldLayout,
        VkImageLayout         newLayout,
        VkPipelineStageFlags2 srcStage,
        VkAccessFlags2        srcAccess,
        VkPipelineStageFlags2 dstStage,
        VkAccessFlags2        dstAccess
    ) const noexcept;

    [[nodiscard]] constexpr auto Handle() const noexcept -> VkImage {
        return image;
    }
    [[nodiscard]] constexpr auto HasAttachmentView() const noexcept -> bool {
        return attachmentView != VK_NULL_HANDLE;
    }
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return image != VK_NULL_HANDLE;
    }
    constexpr explicit operator bool() const noexcept {
        return Valid();
    }
    [[nodiscard]] constexpr auto Extent2D() const noexcept -> VkExtent2D {
        return {.width = extent.width, .height = extent.height};
    }

    template <VkImageLayout Layout>
    [[nodiscard]] constexpr auto Assume(VkImageAspectFlags imageAspect) const noexcept -> TypedImage<Layout, VK_FORMAT_UNDEFINED>;
    template <VkImageLayout Layout>
    [[nodiscard]] constexpr auto Assume() const noexcept -> TypedImage<Layout, VK_FORMAT_UNDEFINED>;

    template <VkFormat Format, VkImageLayout Layout>
    [[nodiscard]] constexpr auto MatchFormat(VkImageAspectFlags imageAspect) const noexcept -> std::optional<TypedImage<Layout, Format>>;
    template <VkFormat Format, VkImageLayout Layout>
    [[nodiscard]] constexpr auto MatchFormat() const noexcept -> std::optional<TypedImage<Layout, Format>>;
};

static_assert(std::is_trivially_copyable_v<ImageSlice>);
static_assert(ImageSlice {VK_NULL_HANDLE, VK_NULL_HANDLE, VkExtent2D {7, 8}, VK_FORMAT_UNDEFINED}.extent.depth == 1);

} // namespace ZHLN::Vk
