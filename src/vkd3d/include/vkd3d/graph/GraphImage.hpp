// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ZHLN::VkD3D graph image tags -- the compile-time resource vocabulary the
// frame graph analyses over. A tag is pure data: a name, a neutral format,
// a neutral aspect, and the allocation flags the graph/target machinery
// reads. No API value or handle appears here; the Vulkan target layer
// (src/vulkan/memory/RenderTarget.hpp) converts these spellings at its own
// boundary.

#include <vkd3d/Enums.hpp>
#include <vkd3d/Color.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ZHLN::VkD3D {

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

/// Format-to-aspect derivation is graph theory, not backend knowledge: which
/// plane a format addresses is the same fact on every API, so it lives next
/// to the neutral vocabulary (mirroring GetLayoutSyncInfo in Barriers.hpp).
/// The Vulkan-native spelling in src/vulkan/memory/ImageView.inl retires
/// when the memory layer migrates.
[[nodiscard]] constexpr auto GetFormatAspect(Format format) noexcept -> ImageAspect {
    switch (format) {
        case Format::Undefined: return ImageAspect::None;
        case Format::D16Unorm:
        case Format::X8D24UnormPack32:
        case Format::D32Sfloat:
            return ImageAspect::Depth;
        case Format::S8Uint:
            return ImageAspect::Stencil;
        case Format::D16UnormS8Uint:
        case Format::D24UnormS8Uint:
        case Format::D32SfloatS8Uint:
            return ImageAspect::Depth | ImageAspect::Stencil;
        default:
            return ImageAspect::Color;
    }
}

template <
    ResourceName     Name,
    Format           Fmt,
    ImageAspect      Aspect       = GetFormatAspect(Fmt),
    bool             IsSwapchain  = false,
    bool             IsPersistent = false,
    uint32_t         ScaleDivisor = 1,
    bool             Is3D         = false,
    bool             IsMipmapped  = false>
struct GraphImage {
    static constexpr auto          name          = Name;
    static constexpr Format        format        = Fmt;
    static constexpr ImageAspect   aspect        = Aspect;
    static constexpr bool          is_swapchain  = IsSwapchain;
    static constexpr bool          is_persistent = IsPersistent;
    static constexpr uint32_t      scale_divisor = ScaleDivisor;
    static constexpr bool          is_3d         = Is3D;
    static constexpr bool          is_mipmapped  = IsMipmapped;
};

template <typename Tag>
struct ClearColorOf {
    static constexpr Color4 value = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
};

} // namespace ZHLN::VkD3D
