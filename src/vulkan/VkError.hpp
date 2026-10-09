// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

/* PREAMBLE
 * ErrorCode lives in Zahlen/Core and is the error channel of every layer,
 * src/vulkan included. VkResult is a foreign code (it has a 0 enumerator,
 * which ErrorCode forbids), so raw VkResults convert through ToError at the
 * point of the Vulkan call; a device-lost result is reported as
 * DeviceFault::Lost. The render layer maps that to FrameResult::DeviceLost.
 */

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include "PresentationOutcome.hpp"
#include <Zahlen/Core/ErrorCode.hpp>
#include <cstdint>
#include <string_view>
#include <vulkan/vulkan_core.h>

namespace ZHLN::Vk {

inline constexpr uint32_t      kCategoryHash = Hash32("ZHLN::Vk::Error");
inline constexpr ErrorCategory kCategory     = {
    .name      = "Vk",
    .to_string = [](const uint32_t v) noexcept -> std::string_view { return Reflect::EnumToString(static_cast<VkResult>(v)); },
    .to_name   = [](const uint32_t v) noexcept -> std::string_view { return Reflect::EnumToString(static_cast<VkResult>(v)); },
};

[[nodiscard]] constexpr auto ToError(VkResult result) noexcept -> ErrorCode {
    if (result == VK_ERROR_DEVICE_LOST) {
        return ErrorCode {DeviceFault::Lost};
    }
    if !consteval {
        ZHLN::RegisterErrorCategory(kCategoryHash, &kCategory);
    }
    return ErrorCode{kCategoryHash, static_cast<uint32_t>(result)};
}

} // namespace ZHLN::Vk
