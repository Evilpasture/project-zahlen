// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Error.hpp
//
// ZHLN::Error is the *diagnostic* form of the engine's error channel. The
// channel itself -- what std::expected<T, ...> carries, what functions return,
// what crosses a task or a pipeline -- is ZHLN::ErrorCode in
// Zahlen/ErrorCode.hpp: the same two words, without the machinery that turns
// them into text. Error adds that machinery back on demand: Category(),
// Message() and Name() resolve through the process-wide category registry, and
// the enumerator constructor is where a zero-valued error enum is rejected.
//
// Construction and conversion between the two are implicit and free (the bytes
// are identical and both types are trivially copyable), so a code can be
// promoted at the exact boundary where somebody reads it:
//
//     Error err = result.error();      // promotion, 8 bytes
//     Log("{} ({}): {}", err.Category(), err.Name(), err.Message());
//
// Formatting either type with std::format/Println/Log prints the annotated
// message, so `Log("{}", result.error())` also works and promotes internally.
//
// There is no free ToString(). One used to exist, and it was the wrong shape:
// it took Error, ErrorCode or any reflected enum and answered all three with a
// std::string_view under a name that every caller reads as "give me a string".
// For an ErrorCode it built a temporary Error to do it -- safe only because the
// category registry's tables are static, which is a property of today's
// Message(), not something its signature promises -- and for an enum it hid
// which of the two enum spellings a caller wanted. Each spelling has a name:
// format the value (an enum formats as its annotated message, through the
// formatter in Core/Reflection/Utilities.hpp), call Message(), or ask for an
// enumerator's identifier with Reflect::EnumToString.
#pragma once
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace ZHLN {

// Non-constexpr undefined symbol hook: calling this during constant evaluation forces an immediate compile error
extern void ERROR_CODE_CANNOT_BE_ZERO();

// ============================================================================
// Compressed 8-Byte Polymorphic Error Wrapper
// ============================================================================

class Error {
  public:
    constexpr Error() noexcept = default;

    /// Promotion from the plain carrier: the two words already have the right
    /// shape, so this is a copy -- no category lookup happens here, Category()/
    /// Message()/Name() resolve it lazily, when somebody asks for text.
    constexpr Error(ErrorCode code) noexcept: _category_hash(code.category), _value(code.value) {
    }

    /// Demotion: ErrorCode is exactly this state, so a code can go back into
    /// plumbing (or into an expected<T, ErrorCode>) without a round trip.
    [[nodiscard]] constexpr operator ErrorCode() const noexcept {
        return ErrorCode(_category_hash, _value);
    }

    // Implicit constructor from any enum type
    template <typename E>
        requires std::is_enum_v<E>
    constexpr Error(E val) noexcept: _category_hash(TemplatedDetail::HashTypeName(Reflect::TypeName<E>())), _value(static_cast<uint32_t>(val)) {
        static_assert(
            !Reflect::EnumHasValue<E>(0), ZHLN::FormatConst<512>(
                                              R"(
===============================================================================
  [COMPILER ERROR] Error enum '{}' contains an enumerator with value 0!
===============================================================================
  In modern C++, success is represented by an engaged std::expected<T, ErrorCode>.
  Remove 'Success = 0' and start error enumerators at 1 (e.g., FirstError = 1).
===============================================================================
)",
                                              Reflect::TypeName<E>()
                                          )
        );

        if (static_cast<uint32_t>(val) == 0) {
            if consteval {
                // Halts compilation immediately if a 0-valued error is created at compile time
                ERROR_CODE_CANNOT_BE_ZERO();
            } else {
                // Immediate crash if an un-enumerated 0 was dynamically cast to E at runtime
                DebugBreak();
            }
        }

        if consteval {
            // Evaluated at compile-time: registration skipped
        } else {
            // Forces instantiation of the static registration node at runtime, so an
            // Error built directly from an enum is printable too (an ErrorCode built
            // from one registers it in its own constructor; the static is shared).
            [[maybe_unused]] bool dummy = TemplatedDetail::CategoryRegistration<E>::registered;
        }
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
            const auto* cat = TemplatedDetail::ResolveCategory(_category_hash);
            return (cat != nullptr) ? cat->name : "None";
        }
    }

    [[nodiscard]] constexpr auto Message() const noexcept -> std::string_view {
        if consteval {
            return "CompileTimeError";
        } else {
            const auto* cat = TemplatedDetail::ResolveCategory(_category_hash);
            return (cat != nullptr) ? cat->to_string(_value) : "None";
        }
    }

    /// The enumerator identifier ("EngineInitFailed"), where Message() may
    /// return the enumerator's Description annotation instead.
    [[nodiscard]] constexpr auto Name() const noexcept -> std::string_view {
        if consteval {
            return "CompileTimeError";
        } else {
            const auto* cat = TemplatedDetail::ResolveCategory(_category_hash);
            return (cat != nullptr) ? cat->to_name(_value) : "None";
        }
    }

    // Evaluates to true if there is an active error (non-zero)
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

/// The promotion, spelled out where a signature wants to say it: ErrorCode's
/// members are declared in Zahlen/ErrorCode.hpp (which cannot see Error), and
/// defined here, where Error is complete.
inline Error ErrorCode::ToError() const noexcept {
    return Error(*this);
}

} // namespace ZHLN

namespace std {
template <>
struct formatter<ZHLN::Error, char>: formatter<string_view, char> {
    auto format(const ZHLN::Error& err, format_context& ctx) const {
        return formatter<string_view, char>::format(err.Message(), ctx);
    }
};

/// Formatting a code is a logging boundary: it promotes to the rich form so
/// `Log("{}", result.error())` prints the annotated message exactly like
/// formatting a ZHLN::Error does.
///
/// This is the std::format path, and it is the only one. ZHLN::Log and
/// ZHLN::Panic format through std::vformat/make_format_args, so they pick this
/// up; ZHLN::Println and ZHLN::Print go through ZHLN::Format's own AppendValue
/// dispatch (Core/Format.hpp), which has a fixed list of types -- integers,
/// floats, bool, char, anything convertible to string_view, pointers -- and
/// writes "?" for everything else, silently. A code handed to Println has to be
/// spelled `ZHLN::Error(code).Message()` or it prints a question mark.
template <>
struct formatter<ZHLN::ErrorCode, char>: formatter<string_view, char> {
    auto format(const ZHLN::ErrorCode& code, format_context& ctx) const {
        return formatter<string_view, char>::format(ZHLN::Error(code).Message(), ctx);
    }
};
} // namespace std
