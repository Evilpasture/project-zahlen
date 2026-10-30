// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <type_traits>
#include <utility>

#include "../core/Handles.hpp"

namespace ZHLN::Vk {

class Image;
class HeapManager;
struct ImageSlice;

enum class AttachmentViewCreationError : uint8_t {
    OutOfHostMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of host memory">{}) = 1,
    OutOfDeviceMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of device memory">{}),
    CreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Image view creation failed">{}),
    InvalidConfiguration ZHLN_ANNOTATION(ZHLN::Description<"Invalid image view configuration">{}),
};

[[nodiscard]] constexpr auto GetFormatAspect(VkFormat format) noexcept -> VkImageAspectFlags;

// A value-type description of a view onto an image. This is what renderer code
// passes around; the VkImageViewCreateInfo is only synthesized where the
// Vulkan API consumes it (vkCreateImageView, VkImageDescriptorInfoEXT).
class ImageView {
  public:
    static constexpr uint8_t  kRemainingMips   = 0xFF;
    static constexpr uint16_t kRemainingLayers = 0xFFFF;

    constexpr ImageView() noexcept = default;

    constexpr ImageView(
        VkImage            image,
        VkFormat           format,
        VkImageAspectFlags aspect,
        uint8_t            baseMip    = 0,
        uint8_t            mipCount   = kRemainingMips,
        uint16_t           baseLayer  = 0,
        uint16_t           layerCount = 1,
        VkImageViewType    viewType   = VK_IMAGE_VIEW_TYPE_2D
    ) noexcept:
        _image(image),
        _format(format),
        _aspect(aspect),
        _baseLayer(baseLayer),
        _layerCount(layerCount),
        _baseMip(baseMip),
        _mipCount(mipCount),
        _viewType(static_cast<uint8_t>(viewType)),
        _swizzlePacked(0) {} // 0 = Identity swizzle

    // -------------------------------------------------------------------------
    // Observers
    // -------------------------------------------------------------------------
    [[nodiscard]] constexpr auto Handle() const noexcept -> VkImage { return _image; }
    [[nodiscard]] constexpr auto Format() const noexcept -> VkFormat { return _format; }
    [[nodiscard]] constexpr auto Aspect() const noexcept -> VkImageAspectFlags { return _aspect; }
    [[nodiscard]] constexpr auto ViewType() const noexcept -> VkImageViewType {
        return static_cast<VkImageViewType>(_viewType);
    }
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _image != VK_NULL_HANDLE && _format != VK_FORMAT_UNDEFINED;
    }
    explicit constexpr operator bool() const noexcept { return Valid(); }

    [[nodiscard]] constexpr auto SubresourceRange() const noexcept -> VkImageSubresourceRange {
        return {
            .aspectMask     = _aspect,
            .baseMipLevel   = _baseMip,
            .levelCount     = _mipCount == kRemainingMips ? VK_REMAINING_MIP_LEVELS : _mipCount,
            .baseArrayLayer = _baseLayer,
            .layerCount     = _layerCount == kRemainingLayers ? VK_REMAINING_ARRAY_LAYERS : _layerCount,
        };
    }

    // -------------------------------------------------------------------------
    // "Tell, Don't Ask": Slicing & Views
    // -------------------------------------------------------------------------
    [[nodiscard]] constexpr auto Mip(uint8_t level) const noexcept -> ImageView {
        ImageView slice = *this;
        slice._baseMip  = _baseMip + level;
        slice._mipCount = 1;
        return slice;
    }

    [[nodiscard]] constexpr auto Mips(uint8_t base, uint8_t count) const noexcept -> ImageView {
        ImageView slice = *this;
        slice._baseMip  = _baseMip + base;
        slice._mipCount = count;
        return slice;
    }

    [[nodiscard]] constexpr auto Layer(uint16_t layer) const noexcept -> ImageView {
        ImageView slice   = *this;
        slice._baseLayer  = _baseLayer + layer;
        slice._layerCount = 1;
        // Tell: Single layer view collapses array/cube into 2D view automatically
        if (slice._viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY || slice._viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
            slice._viewType = VK_IMAGE_VIEW_TYPE_2D;
        }
        return slice;
    }

    [[nodiscard]] constexpr auto Layers(uint16_t base, uint16_t count) const noexcept -> ImageView {
        ImageView slice   = *this;
        slice._baseLayer  = _baseLayer + base;
        slice._layerCount = count;
        return slice;
    }

    [[nodiscard]] constexpr auto AsCube() const noexcept -> ImageView {
        ImageView slice = *this;
        slice._viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        return slice;
    }

    [[nodiscard]] constexpr auto WithAspect(VkImageAspectFlags aspect) const noexcept -> ImageView {
        ImageView slice = *this;
        slice._aspect   = aspect;
        return slice;
    }

    [[nodiscard]] constexpr auto AllSubresources() const noexcept -> ImageView {
        ImageView slice   = *this;
        slice._baseMip    = 0;
        slice._mipCount   = kRemainingMips;
        slice._baseLayer  = 0;
        slice._layerCount = kRemainingLayers;
        return slice;
    }

    [[nodiscard]] constexpr auto ComputeMipExtent(VkExtent2D baseExtent) const noexcept -> VkExtent2D {
        return {
            .width  = std::max(1u, baseExtent.width >> _baseMip),
            .height = std::max(1u, baseExtent.height >> _baseMip)
        };
    }

    // -------------------------------------------------------------------------
    // "Tell, Don't Ask": Driver & Descriptor Synthesis
    // -------------------------------------------------------------------------
    /// Synthesizes the 80-byte C struct directly onto the stack for API calls
    [[nodiscard]] constexpr auto AsCreateInfo() const noexcept -> VkImageViewCreateInfo {
        VkImageAspectFlags descriptorAspect = _aspect;
        // Combined depth/stencil cannot be sampled together in descriptors; default to depth
        if ((descriptorAspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) ==
            (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
            descriptorAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        }

        return {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext            = nullptr,
            .flags            = 0,
            .image            = _image,
            .viewType         = ViewType(),
            .format           = _format,
            .components       = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY
            },
            .subresourceRange = {
                .aspectMask     = descriptorAspect,
                .baseMipLevel   = _baseMip,
                .levelCount     = _mipCount == kRemainingMips ? VK_REMAINING_MIP_LEVELS : _mipCount,
                .baseArrayLayer = _baseLayer,
                .layerCount     = _layerCount == kRemainingLayers ? VK_REMAINING_ARRAY_LAYERS : _layerCount
            }
        };
    }

    // -------------------------------------------------------------------------
    // "Tell, Don't Ask": Synchronization Command Injection
    // -------------------------------------------------------------------------
    /// Deduces stages and accesses from LayoutTraits<OldLayout> / LayoutTraits<NewLayout>.
    template <VkImageLayout OldLayout, VkImageLayout NewLayout>
    void Transition(VkCommandBuffer cmd) const noexcept;

    /// Discards existing contents (VK_IMAGE_LAYOUT_UNDEFINED -> TargetLayout).
    template <VkImageLayout TargetLayout>
    void DiscardTo(VkCommandBuffer cmd) const noexcept;

    /// Runtime layout transition deriving conservative sync masks from oldLayout / newLayout.
    void Transition(VkCommandBuffer cmd, VkImageLayout oldLayout, VkImageLayout newLayout) const noexcept;

    /// Explicit stage/access override without exposing raw handle or subresource range.
    void Transition(
        VkCommandBuffer       cmd,
        VkImageLayout         oldLayout,
        VkImageLayout         newLayout,
        VkPipelineStageFlags2 srcStage,
        VkAccessFlags2        srcAccess,
        VkPipelineStageFlags2 dstStage,
        VkAccessFlags2        dstAccess
    ) const noexcept;

    friend constexpr auto operator==(const ImageView&, const ImageView&) noexcept -> bool = default;

  private:
    VkImage            _image         = VK_NULL_HANDLE; // 8 bytes
    VkFormat           _format        = VK_FORMAT_UNDEFINED; // 4 bytes
    VkImageAspectFlags _aspect        = 0;              // 4 bytes
    uint16_t           _baseLayer     = 0;              // 2 bytes
    uint16_t           _layerCount    = 1;              // 2 bytes
    uint8_t            _baseMip       = 0;              // 1 byte
    uint8_t            _mipCount      = 1;              // 1 byte
    uint8_t            _viewType      = 1;              // 1 byte (2D)
    uint8_t            _swizzlePacked = 0;              // 1 byte
};

static_assert(sizeof(ImageView) == 24, "ImageView must be exactly 24 bytes.");
static_assert(std::is_trivially_copyable_v<ImageView>);

class AttachmentView {
  public:
    AttachmentView() noexcept = default;

    AttachmentView(const AttachmentView&)                    = delete;
    auto operator=(const AttachmentView&) -> AttachmentView& = delete;

    AttachmentView(AttachmentView&& other) noexcept:
        _handle(std::move(other._handle)), _view(std::exchange(other._view, ImageView {})) {
    }
    auto operator=(AttachmentView&& other) noexcept -> AttachmentView& {
        if (this != &other) {
            _handle = std::move(other._handle);
            _view   = std::exchange(other._view, ImageView {});
        }
        return *this;
    }

    [[nodiscard]] auto Get() const noexcept -> VkImageView { return _handle.Get(); }

    /// The value-type description this view was created from; no VkImageViewCreateInfo is stored.
    [[nodiscard]] constexpr auto View() const noexcept -> const ImageView& { return _view; }
    [[nodiscard]] constexpr auto Format() const noexcept -> VkFormat { return _view.Format(); }
    [[nodiscard]] constexpr auto AspectFlags() const noexcept -> VkImageAspectFlags { return _view.Aspect(); }
    [[nodiscard]] auto Valid() const noexcept -> bool { return _handle.Valid(); }
    explicit operator bool() const noexcept { return Valid(); }
    [[nodiscard]] auto Release() noexcept -> VkImageView {
        _view = {};
        return _handle.Release();
    }

  private:
    friend class Image;

    [[nodiscard]] static auto Create(VkDevice device, ImageView view) -> std::expected<AttachmentView, ErrorCode>;

    AttachmentView(VkDevice device, VkImageView handle, ImageView view) noexcept: _handle(device, handle), _view(view) {
    }

    AttachmentViewHandle _handle;
    ImageView            _view {};
};

}

#include "ImageView.inl"
