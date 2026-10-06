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

class Image;
class ResourceWriteBatch;
class HeapManager;
struct ImageSlice;

enum class ImageViewKind : uint8_t {
    Inferred,
    Texture2D,
    Texture2DArray,
    Texture3D,
    Cube,
    CubeArray,
};

enum class ImageAspect : uint8_t {
    Inferred,
    Color,
    Depth,
    Stencil,
    DepthStencil,
};

// Describes the visible subresources of an image. A zero count selects the
// remaining mips or layers; it does not describe the image's current layout.
struct ImageViewConfig {
    ImageViewKind kind = ImageViewKind::Inferred;
    ImageAspect   aspect = ImageAspect::Inferred;
    uint32_t      baseMip = 0;
    uint32_t      mipCount = 0;
    uint32_t      baseLayer = 0;
    uint32_t      layerCount = 0;
};

enum class ImageViewCreationError : uint8_t {
    OutOfHostMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of host memory">{}) = 1,
    OutOfDeviceMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of device memory">{}),
    CreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Image view creation failed">{}),
    InvalidConfiguration ZHLN_ANNOTATION(ZHLN::Description<"Invalid image view configuration">{}),
};

[[nodiscard]] constexpr auto GetFormatAspect(VkFormat format) noexcept -> VkImageAspectFlags;

class ImageView {
  public:
    ImageView() noexcept = default;

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
    [[nodiscard]] constexpr auto Format() const noexcept -> VkFormat { return _info.format; }
    [[nodiscard]] constexpr auto AspectFlags() const noexcept -> VkImageAspectFlags { return _info.subresourceRange.aspectMask; }
    [[nodiscard]] auto Valid() const noexcept -> bool { return _handle.Valid(); }
    explicit operator bool() const noexcept { return Valid(); }
    [[nodiscard]] auto Release() noexcept -> VkImageView {
        _info = {};
        return _handle.Release();
    }

  private:
    friend class Image;
    friend class ResourceWriteBatch;
    friend class HeapManager;
    friend struct ImageSlice;

    [[nodiscard]] auto CreateInfo() const noexcept -> const VkImageViewCreateInfo& { return _info; }
    [[nodiscard]] static auto Create(VkDevice device, const VkImageViewCreateInfo& info) -> std::expected<ImageView, ErrorCode>;

    ImageView(VkDevice device, VkImageView view, const VkImageViewCreateInfo& info) noexcept:
        _handle(device, view), _info(info) {
    }

    ImageViewHandle       _handle;
    VkImageViewCreateInfo _info {};
};

}
#include "ImageView.inl"
