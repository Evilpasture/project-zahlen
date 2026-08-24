// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <expected>

namespace ZHLN::Vk {

// ============================================================================
// Image View Helpers
// ============================================================================

[[nodiscard]] constexpr auto GetFormatAspect(VkFormat format) noexcept -> VkImageAspectFlags;

[[nodiscard]] auto CreateView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mips = 1)
    -> std::expected<ImageView, VkResult>;

template <VkFormat F>
[[nodiscard]] auto CreateView(VkDevice device, VkImage image, VkImageAspectFlags aspect = GetFormatAspect(F), uint32_t mips = 1)
    -> std::expected<ImageView, VkResult>;

template <VkFormat F>
[[nodiscard]] auto CreateView3D(VkDevice device, VkImage image, VkImageAspectFlags aspect, uint32_t mips)
    -> std::expected<ImageView, VkResult>;

template <VkFormat F>
[[nodiscard]] auto CreateViewCube(VkDevice device, VkImage image, uint32_t mips = 1)
    -> std::expected<ImageView, VkResult>;

template <VkFormat F>
[[nodiscard]] auto CreateView2DArray(
    VkDevice           device,
    VkImage            image,
    uint32_t           baseLayer,
    uint32_t           layerCount,
    VkImageAspectFlags aspect = GetFormatAspect(F),
    uint32_t           mips   = 1
) -> std::expected<ImageView, VkResult>;

template <VkFormat F>
[[nodiscard]] auto
    CreateViewCubeArray(VkDevice device, VkImage image, uint32_t arrayLayers, VkImageAspectFlags aspect = GetFormatAspect(F), uint32_t mips = 1)
        -> std::expected<ImageView, VkResult>;

template <VkFormat F>
[[nodiscard]] auto CreateViewSingleMip(VkDevice device, VkImage image, uint32_t baseMip, VkImageAspectFlags aspect = GetFormatAspect(F))
    -> std::expected<ImageView, VkResult>;

} // namespace ZHLN::Vk
#include "ImageView.inl"