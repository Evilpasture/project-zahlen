// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../memory/ImageView.hpp"
#include <array>
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

template <
    ResourceName       Name,
    VkFormat           Format,
    VkImageAspectFlags Aspect       = GetFormatAspect(Format),
    bool               IsSwapchain  = false,
    bool               IsPersistent = false,
    uint32_t           ScaleDivisor = 1,
    bool               Is3D         = false,
    bool               IsMipmapped  = false>
struct GraphImage {
    static constexpr auto               name          = Name;
    static constexpr VkFormat           format        = Format;
    static constexpr VkImageAspectFlags aspect        = Aspect;
    static constexpr bool               is_swapchain  = IsSwapchain;
    static constexpr bool               is_persistent = IsPersistent;
    static constexpr uint32_t           scale_divisor = ScaleDivisor;
    static constexpr bool               is_3d         = Is3D;
    static constexpr bool               is_mipmapped  = IsMipmapped;
};

} // namespace ZHLN::Vk
