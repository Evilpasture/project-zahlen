// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan/VulkanHeader.hpp"

#include "vulkan/FormatTag.hpp"
#include "vulkan/memory/ImageView.hpp"
#include "vulkan/graph/DynamicRendering.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include "vulkan/core/RenderCore.hpp"

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

// The VkFormat a graph image is created with and the aspect its views address
// both come from the format tag rather than from the resource declaration, so a
// resource cannot state an aspect that disagrees with its own format -- there is
// one place the answer lives. IsFormat is the table's own definition of what a
// tag is; it lives in FormatTag.hpp precisely so this header can name it without
// pulling in FormatTags.hpp (which needs the graph's TypeList).

template <
    ResourceName       Name,
    Format::IsFormat FormatTag,
    bool               IsSwapchain  = false,
    bool               IsPersistent = false,
    uint32_t           ScaleDivisor = 1,
    bool               Is3D         = false,
    bool               IsMipmapped  = false>
struct GraphImage {
    static constexpr auto               name          = Name;
    static constexpr VkFormat           format        = FormatTag::vk;
    static constexpr VkImageAspectFlags aspect        = FormatTag::aspect;
    static constexpr bool               is_swapchain  = IsSwapchain;
    static constexpr bool               is_persistent = IsPersistent;
    static constexpr uint32_t           scale_divisor = ScaleDivisor;
    static constexpr bool               is_3d         = Is3D;
    static constexpr bool               is_mipmapped  = IsMipmapped;

    // The tag this target was built from, for code that needs the format as a
    // type (bytes_per_pixel, is_depth_stencil) rather than as a VkFormat value.
    using format_tag = FormatTag;
};

template <typename Tag>
struct ClearColorOf {
    static constexpr Color4 value = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
};

} // namespace ZHLN::Vk
