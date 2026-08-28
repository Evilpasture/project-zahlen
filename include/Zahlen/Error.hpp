// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Error.hpp
#pragma once
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Core/String.hpp>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace ZHLN {

// Non-constexpr undefined symbol hook: calling this during constant evaluation forces an immediate compile error
extern void ERROR_CODE_CANNOT_BE_ZERO();

struct ErrorCategory {
    std::string_view name;
    std::string_view (*to_string)(uint32_t) noexcept;
};

namespace detail {

constexpr auto HashTypeName(std::string_view str) noexcept -> uint32_t {
    uint32_t hash = 2166136261u;
    for (char c: str) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

template <typename E>
    requires std::is_enum_v<E>
inline auto GetCategoryInstance() noexcept -> const ErrorCategory* {
    // Force compiler instantiation of EnumToString<E> via immediate invocation to prevent link-time undefined symbol errors in Clang
    [[maybe_unused]] auto dummy = Reflect::EnumToString(E {});
    // Same workaround for EnumToMessage<E>: on some Clang P2996 builds the
    // runtime call inside the category lambda below does not get a weak
    // definition emitted in this TU (observed as an arm64/macOS link failure
    // with undefined EnumToMessage<...Error> symbols); the immediate
    // invocation forces the instantiation and emission here.
    [[maybe_unused]] auto dummyMessage = Reflect::EnumToMessage(E {});

    static constexpr ErrorCategory cat = {.name = Reflect::TypeName<E>(), .to_string = [](uint32_t val) noexcept -> std::string_view {
                                              // Using abstracted EnumToMessage to fetch annotations, falling back to string names
                                              return Reflect::EnumToMessage(static_cast<E>(val));
                                          }};
    return &cat;
}

struct RegistryNode {
    uint32_t             hash;
    const ErrorCategory* category;
    RegistryNode*        next;
};

// Safe construct-on-first-use singleton to avoid Static Initialization Order Fiasco
inline auto GetRegistryHead() noexcept -> std::atomic<RegistryNode*>& {
    static std::atomic<RegistryNode*> head {nullptr};
    return head;
}

template <typename E>
    requires std::is_enum_v<E>
struct CategoryRegistration {
    static inline RegistryNode node = {.hash = HashTypeName(ZHLN::Reflect::TypeName<E>()), .category = GetCategoryInstance<E>(), .next = nullptr};

    // Thread-safe lock-free category registration
    static inline bool registered = []() -> auto {
        auto&         head     = GetRegistryHead();
        RegistryNode* expected = head.load(std::memory_order::relaxed);
        do {
            node.next = expected;
        } while (!head.compare_exchange_weak(expected, &node, std::memory_order::release, std::memory_order::relaxed));
        return true;
    }();
};

inline auto ResolveCategory(uint32_t hash) noexcept -> const ErrorCategory* {
    RegistryNode* curr = GetRegistryHead().load(std::memory_order::acquire);
    while (curr != nullptr) {
        if (curr->hash == hash) {
            return curr->category;
        }
        curr = curr->next;
    }
    return nullptr;
}

} // namespace detail

// ============================================================================
// Compressed 8-Byte Polymorphic Error Wrapper
// ============================================================================

class Error {
  public:
    constexpr Error() noexcept = default;

    // Implicit constructor from any enum type
    template <typename E>
        requires std::is_enum_v<E>
    constexpr Error(E val) noexcept: _category_hash(detail::HashTypeName(Reflect::TypeName<E>())), _value(static_cast<uint32_t>(val)) {
        static_assert(
            !Reflect::EnumHasValue<E>(0), ZHLN::FormatConst<512>(
                                              R"(
===============================================================================
  [COMPILER ERROR] Error enum '{}' contains an enumerator with value 0!
===============================================================================
  In modern C++, success is represented by an engaged std::expected<T, Error>.
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
#if defined(__clang__) || defined(__GNUC__)
                __builtin_trap();
#else
                std::abort();
#endif
            }
        }

        if consteval {
            // Evaluated at compile-time: registration skipped
        } else {
            // Forces instantiation of the static registration node at runtime
            [[maybe_unused]] bool dummy = detail::CategoryRegistration<E>::registered;
        }
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr auto Is() const noexcept -> bool {
        return _category_hash == detail::HashTypeName(ZHLN::Reflect::TypeName<E>());
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
            const auto* cat = detail::ResolveCategory(_category_hash);
            return (cat != nullptr) ? cat->name : "None";
        }
    }

    [[nodiscard]] constexpr auto Message() const noexcept -> std::string_view {
        if consteval {
            return "CompileTimeError";
        } else {
            const auto* cat = detail::ResolveCategory(_category_hash);
            return (cat != nullptr) ? cat->to_string(_value) : "None";
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

template <typename T>
constexpr auto ToString(T val) noexcept -> std::string_view {
    if constexpr (std::is_same_v<T, Error>) {
        return val.Message();
    } else if constexpr (std::is_enum_v<T>) {
        return Reflect::EnumToMessage(val);
    } else {
        static_assert(sizeof(T) == 0, "ToString is only defined for Error or reflected Enums.");
        return "";
    }
}

} // namespace ZHLN

namespace std {
template <>
struct formatter<ZHLN::Error, char>: formatter<string_view, char> {
    auto format(const ZHLN::Error& err, format_context& ctx) const {
        return formatter<string_view, char>::format(err.Message(), ctx);
    }
};
} // namespace std
