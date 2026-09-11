// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <expected>

namespace ZHLN::Vk {

// ============================================================================
// Image View Helpers
// ============================================================================

[[nodiscard]] constexpr auto GetFormatAspect(VkFormat format) noexcept -> VkImageAspectFlags;

[[nodiscard]] constexpr auto MakeViewCreateInfo(
    VkImage            image,
    VkFormat           format,
    VkImageViewType    viewType,
    VkImageAspectFlags aspect,
    uint32_t           mipLevels   = 1,
    uint32_t           arrayLayers = 1,
    uint32_t           baseMip     = 0,
    uint32_t           baseLayer   = 0
) noexcept -> VkImageViewCreateInfo;

[[nodiscard]] constexpr auto MakeViewCreateInfo2D(
    VkImage image, VkFormat format, uint32_t mipLevels, VkImageAspectFlags aspect, uint32_t baseMip = 0
) noexcept -> VkImageViewCreateInfo;
[[nodiscard]] constexpr auto MakeViewCreateInfo3D(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mipLevels = 1) noexcept
    -> VkImageViewCreateInfo;
[[nodiscard]] constexpr auto MakeViewCreateInfoCube(VkImage image, VkFormat format, uint32_t mipLevels) noexcept -> VkImageViewCreateInfo;
[[nodiscard]] constexpr auto MakeViewCreateInfo2DArray(
    VkImage            image,
    VkFormat           format,
    uint32_t           baseLayer,
    uint32_t           layerCount,
    VkImageAspectFlags aspect,
    uint32_t           mipLevels,
    uint32_t           baseMip = 0
) noexcept -> VkImageViewCreateInfo;
[[nodiscard]] constexpr auto MakeViewCreateInfoCubeArray(
    VkImage image, VkFormat format, uint32_t arrayLayers, VkImageAspectFlags aspect, uint32_t mipLevels = 1
) noexcept -> VkImageViewCreateInfo;

[[nodiscard]] auto CreateView(VkDevice device, const VkImageViewCreateInfo& info) -> std::expected<ImageView, Error>;
[[nodiscard]] auto CreateView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mips = 1) -> std::expected<ImageView, Error>;

template <VkFormat F>
[[nodiscard]] auto
    CreateView(VkDevice device, VkImage image, VkImageAspectFlags aspect = GetFormatAspect(F), uint32_t mips = 1) -> std::expected<ImageView, Error>;

template <VkFormat F>
[[nodiscard]] auto CreateView3D(VkDevice device, VkImage image, VkImageAspectFlags aspect, uint32_t mips) -> std::expected<ImageView, Error>;

template <VkFormat F>
[[nodiscard]] auto CreateViewCube(VkDevice device, VkImage image, uint32_t mips = 1) -> std::expected<ImageView, Error>;

template <VkFormat F>
[[nodiscard]] auto CreateView2DArray(
    VkDevice           device,
    VkImage            image,
    uint32_t           baseLayer,
    uint32_t           layerCount,
    VkImageAspectFlags aspect = GetFormatAspect(F),
    uint32_t           mips   = 1
) -> std::expected<ImageView, Error>;

template <VkFormat F>
[[nodiscard]] auto CreateViewCubeArray(VkDevice device, VkImage image, uint32_t arrayLayers, VkImageAspectFlags aspect = GetFormatAspect(F), uint32_t mips = 1)
    -> std::expected<ImageView, Error>;

template <VkFormat F>
[[nodiscard]] auto
    CreateViewSingleMip(VkDevice device, VkImage image, uint32_t baseMip, VkImageAspectFlags aspect = GetFormatAspect(F)) -> std::expected<ImageView, Error>;

} // namespace ZHLN::Vk
#include "ImageView.inl"
