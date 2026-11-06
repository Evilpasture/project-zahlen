// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../memory/ImageView.hpp"
#include "DynamicRendering.hpp"
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ZHLN::Vk {

template <size_t N>
struct ResourceName {
    std::array<char, N> value {};

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
    constexpr ResourceName(const char (&str)[N]) {
        for (size_t i = 0; i < N; ++i) {
            value[i] = str[i];
        }
    }

    [[nodiscard]] constexpr std::string_view string_view() const noexcept {
        return std::string_view(value.data(), N > 0 && value[N - 1] == '\0' ? N - 1 : N);
    }
};

// What a graph image needs from a format: the VkFormat it is created with, and
// the aspect its views address. Both come from the format tag rather than from
// the resource declaration, so a resource cannot state an aspect that disagrees
// with its own format -- there is one place the answer lives.
//
// Spelled structurally instead of naming IsFormat because FormatTags.hpp
// includes graph/RenderPassDecl.hpp, which includes this header: the concept is
// not reachable from here without a cycle. FormatTags.hpp stays authoritative
// (IsFormat, ColorFormat, DepthStencilFormat, and the table-wide assertions that
// every tag's aspect agrees with its is_depth_stencil flag). The requirement
// itself is written the way IsFormat writes its members -- convertible_to on the
// named member -- so the two cannot drift apart in idiom either.
template <typename T>
concept ImageFormatTag = requires {
    { T::vk } -> std::convertible_to<VkFormat>;
    { T::aspect } -> std::convertible_to<VkImageAspectFlags>;
};

template <
    ResourceName       Name,
    ImageFormatTag     Format,
    bool               IsSwapchain  = false,
    bool               IsPersistent = false,
    uint32_t           ScaleDivisor = 1,
    bool               Is3D         = false,
    bool               IsMipmapped  = false>
struct GraphImage {
    static constexpr auto               name          = Name;
    static constexpr VkFormat           format        = Format::vk;
    static constexpr VkImageAspectFlags aspect        = Format::aspect;
    static constexpr bool               is_swapchain  = IsSwapchain;
    static constexpr bool               is_persistent = IsPersistent;
    static constexpr uint32_t           scale_divisor = ScaleDivisor;
    static constexpr bool               is_3d         = Is3D;
    static constexpr bool               is_mipmapped  = IsMipmapped;

    // The tag this target was built from, for code that needs the format as a
    // type (bytes_per_pixel, is_depth_stencil) rather than as a VkFormat value.
    using format_tag = Format;
};

template <typename Tag>
struct ClearColorOf {
    static constexpr Color4 value = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
};

} // namespace ZHLN::Vk
