// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN::Vk {

// ============================================================================
// Image View Helpers
// ============================================================================

[[nodiscard]] constexpr auto GetFormatAspect(VkFormat format) noexcept -> VkImageAspectFlags;

template <VkFormat F>
[[nodiscard]] auto CreateView(VkDevice device, VkImage image, VkImageAspectFlags aspect = GetFormatAspect(F), uint32_t mips = 1) -> ImageView;

template <VkFormat F>
inline auto CreateView3D(VkDevice device, VkImage image, VkImageAspectFlags aspect, uint32_t mips) -> ImageView {
    ZHLN_ImageViewDesc desc = {
        .image            = image,
        .format           = F,
        .aspect           = aspect,
        .mip_levels       = mips,
        .array_layers     = 1,
        .view_type        = VK_IMAGE_VIEW_TYPE_3D, // Standard 3D Type
        .base_array_layer = 0,
    };
    VkImageView view = ZHLN_CreateImageView(device, &desc);
    return ImageView {device, view};
}

template <VkFormat F>
[[nodiscard]] auto CreateViewCube(VkDevice device, VkImage image, uint32_t mips = 1) -> ImageView;

template <VkFormat F>
[[nodiscard]] auto CreateView2DArray(
    VkDevice           device,
    VkImage            image,
    uint32_t           baseLayer,
    uint32_t           layerCount,
    VkImageAspectFlags aspect = GetFormatAspect(F),
    uint32_t           mips   = 1
) -> ImageView;

template <VkFormat F>
[[nodiscard]] auto
    CreateViewCubeArray(VkDevice device, VkImage image, uint32_t arrayLayers, VkImageAspectFlags aspect = GetFormatAspect(F), uint32_t mips = 1) -> ImageView;
} // namespace ZHLN::Vk
#include "ImageView.inl"
