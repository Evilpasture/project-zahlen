// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ZHLN::VkD3D -- which image aspect a format implies.
//
// A format names a pixel layout; an aspect names the part of an image a view or
// a barrier addresses (color, depth, stencil). The derivation is a property of
// the format itself, not of the API that stores it: depth formats produce depth
// aspects, packed depth-stencil formats produce both, everything else is color.
// Both backends need the same answer, so it is stated once, here.
//
// This mirrors ZHLN::Vk::GetFormatAspect (src/vulkan/memory/ImageView.inl) rule
// for rule. The two cannot be merged while Vk::Format still carries the native
// VK_FORMAT_* values: that is a different enum from Format, and the conversion
// between them is FromVk(). Once call sites are on VkD3D::Format the Vulkan
// helper becomes a two-line adapter over this one instead of a second table.

#include <vkd3d/Enums.hpp>

#include <cstdint>

namespace ZHLN::VkD3D {

[[nodiscard]] constexpr auto GetFormatAspect(Format format) noexcept -> ImageAspect {
    switch (format) {
        case Format::Undefined: return ImageAspect::None;
        case Format::D16Unorm:
        case Format::X8D24UnormPack32:
        case Format::D32Sfloat: return ImageAspect::Depth;
        case Format::S8Uint: return ImageAspect::Stencil;
        case Format::D16UnormS8Uint:
        case Format::D24UnormS8Uint:
        case Format::D32SfloatS8Uint: return ImageAspect::Depth | ImageAspect::Stencil;
        default: return ImageAspect::Color;
    }
}

} // namespace ZHLN::VkD3D
