// include/Zahlen/Error.hpp
#pragma once
#include "../../src/detail/Reflection.hpp"
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace ZHLN {

// ============================================================================
// Type-Erased Error Category Interface
// ============================================================================

struct ErrorCategory {
    std::string_view name;
    std::string_view (*to_string)(uint32_t) noexcept;
};

namespace detail {

template <typename E>
    requires std::is_enum_v<E>
consteval const ErrorCategory* GetCategory() noexcept {
    static constexpr ErrorCategory cat = {.name = ZHLN::Reflect::TypeName<E>(), .to_string = [](uint32_t val) noexcept -> std::string_view {
                                              return ZHLN::Reflect::EnumToString(static_cast<E>(val));
                                          }};
    return &cat;
}

} // namespace detail

// ============================================================================
// Unified Polymorphic Error Wrapper (Enables Enum "Inheritance")
// ============================================================================

class Error {
  public:
    // Default-constructed Error represents Success
    constexpr Error() noexcept = default;

    // Implicit constructor from any enum type, functioning as derived classes
    template <typename E>
        requires std::is_enum_v<E>
    constexpr Error(E val) noexcept: _category(detail::GetCategory<E>()), _value(static_cast<uint32_t>(val)) {
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr bool Is() const noexcept {
        return _category == detail::GetCategory<E>();
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr bool Is(E val) const noexcept {
        return Is<E>() && _value == static_cast<uint32_t>(val);
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr E As() const noexcept {
        return static_cast<E>(_value);
    }

    [[nodiscard]] constexpr std::string_view Category() const noexcept {
        return _category ? _category->name : "Success";
    }

    [[nodiscard]] constexpr std::string_view Message() const noexcept {
        return _category ? _category->to_string(_value) : "Success";
    }

    // Evaluates to true if there is an active error (non-success)
    constexpr explicit operator bool() const noexcept {
        return _value != 0;
    }

    constexpr bool operator==(const Error& other) const noexcept = default;

  private:
    const ErrorCategory* _category = nullptr;
    uint32_t             _value    = 0;
};

// ============================================================================
// Single Unified Generic ToString Overload for both Error and raw Enums
// ============================================================================

template <typename T>
constexpr std::string_view ToString(T val) noexcept {
    if constexpr (std::is_same_v<T, Error>) {
        return val.Message();
    } else if constexpr (std::is_enum_v<T>) {
        return ZHLN::Reflect::EnumToString(val);
    } else {
        static_assert(sizeof(T) == 0, "ToString is only defined for Error or reflected Enums.");
        return "";
    }
}

} // namespace ZHLN
