// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <array>
#include <cstdint>
#include <expected>
#include <utility>

#include "../core/Handles.hpp"

namespace ZHLN::Vk {

[[nodiscard]] constexpr auto GetFormatAspect(VkFormat format) noexcept -> VkImageAspectFlags;

// Descriptor heaps need the exact view create info, not only the VkImageView
// handle. Keep it with the owned view so moves cannot separate the two.
class ImageView {
  public:
    ImageView() noexcept = default;
    ImageView(VkDevice device, VkImageView view, const VkImageViewCreateInfo& info) noexcept:
        _handle(device, view), _info(info) {
    }

    ImageView(const ImageView&)                    = delete;
    auto operator=(const ImageView&) -> ImageView& = delete;

    ImageView(ImageView&& other) noexcept:
        _handle(std::move(other._handle)), _info(std::exchange(other._info, VkImageViewCreateInfo {})) {
    }
    auto operator=(ImageView&& other) noexcept -> ImageView& {
        if (this != &other) {
            _handle = std::move(other._handle);
            _info = std::exchange(other._info, VkImageViewCreateInfo {});
        }
        return *this;
    }

    [[nodiscard]] auto Get() const noexcept -> VkImageView { return _handle.Get(); }
    [[nodiscard]] auto Info() const noexcept -> const VkImageViewCreateInfo& { return _info; }
    [[nodiscard]] auto Valid() const noexcept -> bool { return _handle.Valid(); }
    explicit operator bool() const noexcept { return Valid(); }
    [[nodiscard]] auto Release() noexcept -> VkImageView {
        _info = {};
        return _handle.Release();
    }

    [[nodiscard]] static auto Create(VkDevice device, const VkImageViewCreateInfo& info) -> std::expected<ImageView, ErrorCode>;
    [[nodiscard]] static auto Create(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mips = 1)
        -> std::expected<ImageView, ErrorCode>;

    template <VkFormat F>
    [[nodiscard]] static auto Create(VkDevice device, VkImage image, VkImageAspectFlags aspect = GetFormatAspect(F), uint32_t mips = 1)
        -> std::expected<ImageView, ErrorCode>;

    template <VkFormat F>
    [[nodiscard]] static auto Create3D(VkDevice device, VkImage image, VkImageAspectFlags aspect, uint32_t mips) -> std::expected<ImageView, ErrorCode>;

    template <VkFormat F>
    [[nodiscard]] static auto CreateCube(VkDevice device, VkImage image, uint32_t mips = 1) -> std::expected<ImageView, ErrorCode>;

    template <VkFormat F>
    [[nodiscard]] static auto Create2DArray(
        VkDevice           device,
        VkImage            image,
        uint32_t           baseLayer,
        uint32_t           layerCount,
        VkImageAspectFlags aspect = GetFormatAspect(F),
        uint32_t           mips   = 1
    ) -> std::expected<ImageView, ErrorCode>;

    template <VkFormat F>
    [[nodiscard]] static auto CreateCubeArray(
        VkDevice device, VkImage image, uint32_t arrayLayers, VkImageAspectFlags aspect = GetFormatAspect(F), uint32_t mips = 1
    ) -> std::expected<ImageView, ErrorCode>;

    template <VkFormat F>
    [[nodiscard]] static auto CreateSingleMip(VkDevice device, VkImage image, uint32_t baseMip, VkImageAspectFlags aspect = GetFormatAspect(F))
        -> std::expected<ImageView, ErrorCode>;

  private:
    ImageViewHandle _handle;
    VkImageViewCreateInfo _info {};
};

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

}
#include "ImageView.inl"
