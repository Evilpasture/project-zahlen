// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The format tags and the concepts that describe one, with no dependency on the
// graph. FormatTags.hpp is the list over these (AllFormats, Visit, AsVkFormats)
// and it needs TypeList, which lives in graph/RenderPassDecl.hpp; this header is
// what a lower layer can include -- GraphImage.hpp and the attachment plumbing
// both need to name IsFormat and Format::Undefined, and neither can reach
// FormatTags.hpp without closing a cycle.
//
// A render header, so the usual rule applies: Rendering.hpp first, and Vulkan
// comes from VulkanHeader.hpp -> <volk.h>. No <vulkan/vulkan.h> here.
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <concepts>
#include <cstdint>

namespace ZHLN::Vk::Format {

template <typename T>
concept IsFormat = requires {
    { T::vk } -> std::convertible_to<VkFormat>;
    { T::aspect } -> std::convertible_to<VkImageAspectFlags>;
    { T::bytes_per_pixel } -> std::convertible_to<uint32_t>;
    { T::is_depth_stencil } -> std::convertible_to<bool>;
};

namespace TemplatedDetail {

template <typename T>
consteval auto IsUndefined() -> bool {
    return T::vk == VK_FORMAT_UNDEFINED;
}

// A colour format addresses the colour aspect; a depth-stencil format addresses
// depth and/or stencil. Stating both flags and deriving one from the other is
// what keeps the classification below from being a second, free-floating opinion.
template <typename T>
consteval auto IsSelfConsistent() -> bool {
    if constexpr (!IsFormat<T>) {
        return false;
    } else if constexpr (IsUndefined<T>()) {
        return T::aspect == 0 && T::bytes_per_pixel == 0;
    } else if constexpr (T::is_depth_stencil) {
        return (T::aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
    } else {
        return (T::aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
    }
}

} // namespace TemplatedDetail

template <typename T>
concept UndefinedFormatTag = IsFormat<T> && TemplatedDetail::IsUndefined<T>();

// The attachment classifications. These are cross-checks, not the graph's source
// of truth: the graph classifies an image by the *layout* it is bound with
// (IsColorAttachment keys off VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL), because
// layout says how the image takes part in a pass while aspect only says which
// part of it you address. These concepts exist so a declaration can assert that
// the tag it was handed agrees with the role it is being given.
template <typename T>
concept ColorFormat = IsFormat<T> && !TemplatedDetail::IsUndefined<T>() && !T::is_depth_stencil;

template <typename T>
concept DepthStencilFormat = IsFormat<T> && !TemplatedDetail::IsUndefined<T>() && T::is_depth_stencil;

// A format that carries a stencil aspect. This asks the aspect and not
// is_depth_stencil, deliberately: that flag classifies a format as depth-stencil,
// and D32_SFloat sets it while addressing depth alone. The two questions are
// different -- "is this an attachment a depth pass can use" versus "is there a
// stencil plane to bind" -- and only the aspect value answers the second.
template <typename T>
concept HasStencil = IsFormat<T> && (T::aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;

template <typename T>
concept SelfConsistentFormat = IsFormat<T> && TemplatedDetail::IsSelfConsistent<T>();

// ---------------------------------------------------------------------------
// The absence of an attachment
// ---------------------------------------------------------------------------

// Also the "format not known at compile time" marker: TypedImage's primary
// template defaults to it, and the attachment sets use it for a slot that exists
// but has no compile-time format. It is a real VkFormat value
// (VK_FORMAT_UNDEFINED) so it projects into value space like any other tag.
struct Undefined {
    static constexpr VkFormat           vk              = VK_FORMAT_UNDEFINED;
    static constexpr VkImageAspectFlags aspect          = 0;
    static constexpr uint32_t           bytes_per_pixel = 0;
    static constexpr bool               is_depth_stencil = false;
};

} // namespace ZHLN::Vk::Format
