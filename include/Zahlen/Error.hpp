// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace ZHLN {


class Error {
  public:
    constexpr Error() noexcept = default;

    constexpr Error(ErrorCode code) noexcept: _category_hash(code.category), _value(code.value) {
    }

    [[nodiscard]] constexpr operator ErrorCode() const noexcept {
        return ErrorCode(_category_hash, _value);
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr auto Is() const noexcept -> bool {
        return _category_hash == TemplatedDetail::HashTypeName(ZHLN::Reflect::TypeName<E>());
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr auto Is(E val) const noexcept -> bool {
        return Is<E>() && _value == static_cast<uint32_t>(val);
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr auto As() const noexcept -> E {
        return static_cast<E>(_value);
    }

    [[nodiscard]] constexpr auto Category() const noexcept -> std::string_view {
        if consteval {
            return "CompileTimeError";
        } else {
            return TemplatedDetail::LookupErrorStrings(_category_hash, _value).category;
        }
    }

    [[nodiscard]] constexpr auto Message() const noexcept -> std::string_view {
        if consteval {
            return "CompileTimeError";
        } else {
            return TemplatedDetail::LookupErrorStrings(_category_hash, _value).message;
        }
    }

    [[nodiscard]] constexpr auto Name() const noexcept -> std::string_view {
        if consteval {
            return "CompileTimeError";
        } else {
            return TemplatedDetail::LookupErrorStrings(_category_hash, _value).name;
        }
    }

    constexpr explicit operator bool() const noexcept {
        return _value != 0;
    }

    constexpr auto operator==(const Error& other) const noexcept -> bool = default;

  private:
    uint32_t _category_hash = 0;
    uint32_t _value         = 0;
};

static_assert(std::is_standard_layout_v<Error>);
static_assert(std::is_trivially_copyable_v<Error> && std::is_trivially_destructible_v<Error>);
static_assert(sizeof(Error) == 8);

inline Error ErrorCode::ToError() const noexcept {
    return Error(*this);
}

}

namespace std {
template <>
struct formatter<ZHLN::Error, char>: formatter<string_view, char> {
    auto format(const ZHLN::Error& err, format_context& ctx) const {
        return formatter<string_view, char>::format(err.Message(), ctx);
    }
};

template <>
struct formatter<ZHLN::ErrorCode, char>: formatter<string_view, char> {
    auto format(const ZHLN::ErrorCode& code, format_context& ctx) const {
        return formatter<string_view, char>::format(ZHLN::Error(code).Message(), ctx);
    }
};
}
