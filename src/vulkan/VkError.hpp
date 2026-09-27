// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/VkError.hpp
//
// A VkResult that has been decided is a failure. Construction refuses
// VK_SUCCESS -- the channel's zero rule, enforced one layer earlier than
// ErrorCode's -- so everything that reaches the conversion below is a real
// error and crosses as its own category, verbatim: no mirror enum, no
// annotations, no codegen.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <cstdint>
#include <string_view>
#include <vulkan/vulkan_core.h>

namespace ZHLN::Vk {

// Non-constexpr undefined symbol hook, the twin of ERROR_CODE_CANNOT_BE_ZERO:
// calling it during constant evaluation is itself the compile-time diagnostic,
// and the runtime arm below breaks instead, so the function never needs a
// definition.
extern void VK_ERROR_CANNOT_BE_SUCCESS();

inline constexpr uint32_t kCategoryHash = Hash32("ZHLN::Vk::Error");
inline constexpr ErrorCategory kCategory = {
    .name      = "Vk",
    .to_string = [](uint32_t) noexcept -> std::string_view { return "Vulkan error"; },
    .to_name   = [](uint32_t) noexcept -> std::string_view { return "Vulkan error"; },
};

class Error {
  public:
    constexpr explicit Error(const VkResult code) noexcept: _code(code) {
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

    // Same reading as ErrorCode's: true where there *is* an error.
    constexpr explicit operator bool() const noexcept {
        return _code != VK_SUCCESS;
    }

    [[nodiscard]] constexpr auto Is(const VkResult code) const noexcept -> bool {
        return _code == code;
    }

    // Into the error channel, implicit, exactly as expensive as ErrorCode's
    // enum path: a hash and a widening. The category rides along once, on the
    // first conversion, so a later Error promotion prints "Vk" instead of
    // "None" -- without that touch the registry is addressable only by enums.
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
            ::ZHLN::TemplatedDetail::RegisterCategory(kCategoryHash, &kCategory);
            return true;
        }();
    };

    VkResult _code;
};

} // namespace ZHLN::Vk
