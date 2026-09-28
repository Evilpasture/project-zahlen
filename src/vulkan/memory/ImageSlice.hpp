// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <optional>
#include <type_traits>

namespace ZHLN::Vk {

template <VkImageLayout Layout, VkFormat Format>
struct TypedImage;

// A borrowed view of an image. The handles and (when present) the view create
// info belong to the caller; do not keep a slice past destruction or relocation
// of the owning ImageView. Raw WSI/externally-owned views have no metadata.
struct ImageSlice {
    VkImage                      image  = VK_NULL_HANDLE;
    VkImageView                  view   = VK_NULL_HANDLE;
    VkExtent3D                   extent {};
    VkFormat                     format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags           aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageViewCreateInfo* info   = nullptr;

    constexpr ImageSlice() noexcept = default;

    constexpr ImageSlice(VkImage img, VkImageView v, VkExtent2D ext, VkFormat fmt, VkImageAspectFlags asp = VK_IMAGE_ASPECT_COLOR_BIT) noexcept:
        image(img), view(v), extent({ext.width, ext.height, 1}), format(fmt), aspect(asp) {
    }
    constexpr ImageSlice(VkImage img, VkImageView v, VkExtent3D ext, VkFormat fmt, VkImageAspectFlags asp = VK_IMAGE_ASPECT_COLOR_BIT) noexcept:
        image(img), view(v), extent(ext), format(fmt), aspect(asp) {
    }

    ImageSlice(VkImage img, const ImageView& v, VkExtent3D ext, VkFormat fmt) noexcept:
        image(img), view(v.Get()), extent(ext), format(fmt), aspect(v.Info().subresourceRange.aspectMask), info(&v.Info()) {
    }
    ImageSlice(VkImage img, const ImageView& v, VkExtent2D ext, VkFormat fmt) noexcept:
        ImageSlice(img, v, VkExtent3D {ext.width, ext.height, 1}, fmt) {
    }

    [[nodiscard]] constexpr auto Handle() const noexcept -> VkImage {
        return image;
    }
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE;
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

    // Only a matching runtime view format may acquire a compile-time format.
    template <VkFormat Format, VkImageLayout Layout>
    [[nodiscard]] constexpr auto MatchFormat(VkImageAspectFlags imageAspect) const noexcept -> std::optional<TypedImage<Layout, Format>>;
    template <VkFormat Format, VkImageLayout Layout>
    [[nodiscard]] constexpr auto MatchFormat() const noexcept -> std::optional<TypedImage<Layout, Format>>;
};

static_assert(std::is_trivially_copyable_v<ImageSlice>);
static_assert(ImageSlice {VK_NULL_HANDLE, VK_NULL_HANDLE, VkExtent2D {7, 8}, VK_FORMAT_UNDEFINED}.extent.depth == 1);

} // namespace ZHLN::Vk
