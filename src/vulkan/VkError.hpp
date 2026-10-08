// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

/* PREAMBLE
 * src/vulkan is a leaf module: its error channel is Vk::Error, never ErrorCode.
 * Vk::Error carries either a VkResult (under the dedicated "ZHLN::Vk::Error"
 * category, because VkResult has a 0 enumerator and ErrorCode forbids those) or
 * any engine error enum (under that enum's own category), so the whole module
 * reports through one type. The boundary where Vk::Error is converted into the
 * engine's polymorphic ZHLN::ErrorCode belongs exclusively in src/render/
 * (where RenderContext and GeometryManager orchestrate the engine); the
 * conversion reports VK_ERROR_DEVICE_LOST as FrameResult::DeviceLost.
 */

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <Zahlen/Render/FrameResult.hpp>
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
    constexpr Error() noexcept = default;

    constexpr Error(const VkResult code) noexcept: _error(MakeVkErrorCode(code)) {
    }

    template <typename E>
        requires std::is_enum_v<E>
    constexpr Error(const E val) noexcept: _error(val) {
    }

    [[nodiscard]] constexpr auto value() const noexcept -> VkResult {
        return _error.As<VkResult>();
    }

    constexpr explicit operator bool() const noexcept {
        return static_cast<bool>(_error);
    }

    [[nodiscard]] constexpr auto Is(const VkResult code) const noexcept -> bool {
        return _error == ErrorCode {kCategoryHash, static_cast<uint32_t>(code)};
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr auto Is() const noexcept -> bool {
        return _error.Is<E>();
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr auto Is(const E val) const noexcept -> bool {
        return _error.Is(val);
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr auto As() const noexcept -> E {
        return _error.As<E>();
    }

    // The boundary conversion into the engine's polymorphic error channel. A
    // device-lost VkResult is reported as FrameResult::DeviceLost, exactly
    // where the render layer looks for it.
    [[nodiscard]] constexpr operator ErrorCode() const noexcept {
        if consteval {
        } else {
            [[maybe_unused]] bool once = CategoryRide::registered;
        }
        if (Is(VK_ERROR_DEVICE_LOST)) {
            return ErrorCode {FrameResult::DeviceLost};
        }
        return _error;
    }

  private:
    static constexpr auto MakeVkErrorCode(const VkResult code) noexcept -> ErrorCode {
        if (code == VK_SUCCESS) {
            if consteval {
                VK_ERROR_CANNOT_BE_SUCCESS();
            } else {
                DebugBreak();
            }
        }
        return ErrorCode {kCategoryHash, static_cast<uint32_t>(code)};
    }

    struct CategoryRide {
        static inline bool registered = []() -> bool {
            ZHLN::RegisterErrorCategory(kCategoryHash, &kCategory);
            return true;
        }();
    };

    ErrorCode _error;
};

static_assert(sizeof(Error) == sizeof(ErrorCode));
static_assert(std::is_standard_layout_v<Error> && std::is_trivially_copyable_v<Error>);

} // namespace ZHLN::Vk

namespace std {

template <>
struct formatter<ZHLN::Vk::Error, char>: formatter<ZHLN::ErrorCode, char> {
    auto format(const ZHLN::Vk::Error& error, format_context& ctx) const {
        return formatter<ZHLN::ErrorCode, char>::format(static_cast<ZHLN::ErrorCode>(error), ctx);
    }
};

} // namespace std
