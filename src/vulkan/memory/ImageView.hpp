// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
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

enum class AttachmentViewCreationError : uint8_t {
    OutOfHostMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of host memory">{}) = 1,
    OutOfDeviceMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of device memory">{}),
    CreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Image view creation failed">{}),
    InvalidConfiguration ZHLN_ANNOTATION(ZHLN::Description<"Invalid image view configuration">{}),
};

[[nodiscard]] constexpr auto GetFormatAspect(VkFormat format) noexcept -> VkImageAspectFlags;

// The only owning VkImageView wrapper in the engine, and it exists for
// exactly one purpose: dynamic-rendering attachments
// (VkRenderingAttachmentInfo::imageView). Descriptor-heap writes never take
// one of these -- they take the plain VkImageViewCreateInfo produced by
// Image::ViewInfo() / ImageSlice::ViewInfo() instead. If a view is only ever
// sampled or written from a shader, do not create an AttachmentView for it.
class AttachmentView {
  public:
    AttachmentView() noexcept = default;

    AttachmentView(const AttachmentView&)                    = delete;
    auto operator=(const AttachmentView&) -> AttachmentView& = delete;

    AttachmentView(AttachmentView&& other) noexcept:
        _handle(std::move(other._handle)), _info(std::exchange(other._info, VkImageViewCreateInfo {})) {
    }
    auto operator=(AttachmentView&& other) noexcept -> AttachmentView& {
        if (this != &other) {
            _handle = std::move(other._handle);
            _info   = std::exchange(other._info, VkImageViewCreateInfo {});
        }
        return *this;
    }

    [[nodiscard]] auto Get() const noexcept -> VkImageView { return _handle.Get(); }
    // The create-info this handle was built from; used to describe the same
    // subresources when building an ImageSlice. Never a descriptor input by
    // itself -- slice/descriptor code goes through VkImageViewCreateInfo.
    [[nodiscard]] constexpr auto CreateInfo() const noexcept -> const VkImageViewCreateInfo& { return _info; }
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

    [[nodiscard]] static auto Create(VkDevice device, const VkImageViewCreateInfo& info) -> std::expected<AttachmentView, ErrorCode>;

    AttachmentView(VkDevice device, VkImageView view, const VkImageViewCreateInfo& info) noexcept:
        _handle(device, view), _info(info) {
    }

    AttachmentViewHandle  _handle;
    VkImageViewCreateInfo _info {};
};

}
#include "ImageView.inl"
