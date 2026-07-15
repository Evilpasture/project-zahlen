// include/Zahlen/Error.hpp
#pragma once
#include "../../src/detail/Reflection.hpp"
#include <atomic>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace ZHLN {

struct ErrorCategory {
    std::string_view name;
    std::string_view (*to_string)(uint32_t) noexcept;
};

namespace detail {

constexpr uint32_t HashTypeName(std::string_view str) noexcept {
    uint32_t hash = 2166136261u;
    for (char c: str) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

template <typename E>
    requires std::is_enum_v<E>
consteval const ErrorCategory* GetCategoryInstance() noexcept {
    static constexpr ErrorCategory cat = {.name = ZHLN::Reflect::TypeName<E>(), .to_string = [](uint32_t val) noexcept -> std::string_view {
                                              return ZHLN::Reflect::EnumToString(static_cast<E>(val));
                                          }};
    return &cat;
}

struct RegistryNode {
    uint32_t             hash;
    const ErrorCategory* category;
    RegistryNode*        next;
};

// Safe construct-on-first-use singleton to avoid Static Initialization Order Fiasco
inline std::atomic<RegistryNode*>& GetRegistryHead() noexcept {
    static std::atomic<RegistryNode*> head {nullptr};
    return head;
}

template <typename E>
    requires std::is_enum_v<E>
struct CategoryRegistration {
    static inline RegistryNode node = {.hash = HashTypeName(ZHLN::Reflect::TypeName<E>()), .category = GetCategoryInstance<E>(), .next = nullptr};

    // Thread-safe lock-free category registration
    static inline bool registered = []() {
        auto&         head     = GetRegistryHead();
        RegistryNode* expected = head.load(std::memory_order::relaxed);
        do {
            node.next = expected;
        } while (!head.compare_exchange_weak(expected, &node, std::memory_order::release, std::memory_order::relaxed));
        return true;
    }();
};

inline const ErrorCategory* ResolveCategory(uint32_t hash) noexcept {
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
    constexpr Error(E val) noexcept: _category_hash(detail::HashTypeName(ZHLN::Reflect::TypeName<E>())), _value(static_cast<uint32_t>(val)) {
        if consteval {
            // Evaluated at compile-time: registration skipped
        } else {
            // Forces instantiation of the static registration node at runtime
            [[maybe_unused]] bool dummy = detail::CategoryRegistration<E>::registered;
        }
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr bool Is() const noexcept {
        return _category_hash == detail::HashTypeName(ZHLN::Reflect::TypeName<E>());
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
        if consteval {
            return "CompileTimeError";
        } else {
            const auto* cat = detail::ResolveCategory(_category_hash);
            return (cat != nullptr) ? cat->name : "Success";
        }
    }

    [[nodiscard]] constexpr std::string_view Message() const noexcept {
        if consteval {
            return "CompileTimeError";
        } else {
            const auto* cat = detail::ResolveCategory(_category_hash);
            return (cat != nullptr) ? cat->to_string(_value) : "Success";
        }
    }

    // Evaluates to true if there is an active error (non-success)
    constexpr explicit operator bool() const noexcept {
        return _value != 0;
    }

    constexpr bool operator==(const Error& other) const noexcept = default;

  private:
    uint32_t _category_hash = 0; // 4 bytes
    uint32_t _value         = 0; // 4 bytes
};

static_assert(std::is_standard_layout_v<Error>);
static_assert(std::is_trivially_copyable_v<Error> && std::is_trivially_destructible_v<Error>);
static_assert(sizeof(Error) == 8);

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
