// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

/* PREAMBLE
 * src/vulkan should never see ErrorCode.
 * The boundary where Vk::Error is converted into the engine's polymorphic
 * ZHLN::ErrorCode belongs exclusively in src/render/
 * (where RenderContext and GeometryManager orchestrate the engine).
 */

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <cstdint>
#include <string_view>
#include <vulkan/vulkan_core.h>

namespace ZHLN::Vk {

extern void VK_ERROR_CANNOT_BE_SUCCESS();

inline constexpr uint32_t      kCategoryHash = Hash32("ZHLN::Vk::Error");
inline constexpr ErrorCategory kCategory     = {
    .name      = "Vk",
    .to_string = [](const uint32_t v) noexcept -> std::string_view { return Reflect::EnumToString(static_cast<VkResult>(v)); },
    .to_name   = [](const uint32_t v) noexcept -> std::string_view { return Reflect::EnumToString(static_cast<VkResult>(v)); },
};

class Error {
  public:
    constexpr Error(const VkResult code) noexcept: _code(code) {
        if (code == VK_SUCCESS) {
            if consteval {
                VK_ERROR_CANNOT_BE_SUCCESS();
            } else {
                DebugBreak();
            }
        }
    }

    [[nodiscard]] constexpr auto value() const noexcept -> VkResult {
        return _code;
    }

    constexpr explicit operator bool() const noexcept {
        return _code != VK_SUCCESS;
    }

    [[nodiscard]] constexpr auto Is(const VkResult code) const noexcept -> bool {
        return _code == code;
    }

    [[nodiscard]] constexpr operator ErrorCode() const noexcept {
        if consteval {
        } else {
            [[maybe_unused]] bool once = CategoryRide::registered;
        }
        return ErrorCode {kCategoryHash, static_cast<uint32_t>(_code)};
    }

  private:
    struct CategoryRide {
        static inline bool registered = []() -> bool {
            ZHLN::RegisterErrorCategory(kCategoryHash, &kCategory);
            return true;
        }();
    };

    VkResult _code;
};

} // namespace ZHLN::Vk
