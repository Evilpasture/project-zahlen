// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection/Enums.hpp>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>

namespace ZHLN::Reflect {

#if ZHLN_REFLECTION_AVAILABLE

template <typename E>
    requires std::is_enum_v<E>
constexpr auto EnumToFlagsString(E value, std::string& out_buffer) -> std::string_view {
    out_buffer.clear();
    using Under          = std::underlying_type_t<E>;
    const auto val_under = static_cast<Under>(value);

    for (const auto& entry : kEnumEntries<E>) {
        const auto enum_under = static_cast<Under>(entry.value);
        if (enum_under != 0 && (val_under & enum_under) == enum_under) {
            if (!out_buffer.empty()) {
                out_buffer += " | ";
            }
            out_buffer += entry.name;
        }
    }

    if (out_buffer.empty() && val_under == 0) {
        return EnumToString(value);
    }
    return out_buffer;
}

#endif

template <typename E, typename... Args>
    requires std::is_enum_v<E>
inline auto FormatEnumMessage(E value, Args&&... args) -> std::string {
    std::string_view fmt = EnumToMessage(value);
    if constexpr (sizeof...(Args) == 0) {
        return std::string(fmt);
    } else {
        return std::vformat(fmt, std::make_format_args(args...));
    }
}

template <typename E, typename... Args>
    requires std::is_enum_v<E>
inline auto FormatEnumMessageString(E value, Args&&... args) -> std::string {
    std::string_view fmt = EnumToMessage(value);
    if constexpr (sizeof...(Args) == 0) {
        return std::string(fmt);
    } else {
        return std::vformat(fmt, std::make_format_args(args...));
    }
}

} // namespace ZHLN::Reflect

namespace std {

template <typename E>
    requires std::is_enum_v<E>
struct formatter<E, char> : formatter<string_view, char> {
    template <typename FormatContext>
    auto format(E val, FormatContext& ctx) const {
        return formatter<string_view, char>::format(ZHLN::Reflect::EnumToMessage(val), ctx);
    }
};

} // namespace std
