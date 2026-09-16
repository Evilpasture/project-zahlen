// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/ErrorCode.hpp
//
// The engine's error *channel*. Everything fallible returns
// std::expected<T, ZHLN::ErrorCode>: a category hash and an enumerator value,
// two uint32_t and nothing else. Constructing one from an enum costs a
// compile-time type-name hash and the register pair it lives in -- no
// FormatConst expansion, no EnumHasValue scan of the enumerator list, no
// DebugBreak, no per-call reflection work.
//
// ZHLN::Error (Zahlen/Error.hpp) is the *diagnostic* form of the very same two
// words: it resolves the category name and the enumerator's annotated message
// lazily. Promote only where text is actually wanted:
//
//     std::expected<Mesh, ErrorCode> Build();          // plumbing: the carrier
//     if (auto mesh = Build(); !mesh) {
//         const Error err = mesh.error();              // boundary: the rich form
//         Log("mesh build failed: {} ({})", err.Message(), err.Name());
//     }
//
// Both directions are implicit (Error has the matching constructor and
// conversion operator), so neither form has to name the other, and
// ErrorCode::ToError() spells the promotion out where a signature should say it.
//
// This header also owns the process-wide category registry: keyed by the hash
// of the enum's type name, it is what turns a {category, value} pair back into
// text later. An ErrorCode's enum constructor is the only place it *can* be
// populated for a given E -- the registry is addressed by hash, so by the time
// someone asks for Message() the type E is long gone. That touch is the whole
// cost the hot path pays for lazy diagnostics: one relaxed load of an inline
// static bool per conversion, plus one lock-free push the first time each enum
// type is converted. A TU that only includes this header never instantiates
// ZHLN::Error, its zero-value static_assert, or its FormatConst expansion.

#pragma once

#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace ZHLN {

class Error;

// ============================================================================
// Category Registry (shared by ErrorCode and Error)
// ============================================================================

struct ErrorCategory {
    std::string_view name;
    std::string_view (*to_string)(uint32_t) noexcept; // annotation text, falling back to the enumerator name
    std::string_view (*to_name)(uint32_t) noexcept;   // the enumerator identifier itself, always
};

namespace TemplatedDetail {

constexpr auto HashTypeName(std::string_view str) noexcept -> uint32_t {
    return Hash32(str);
}

template <typename E>
    requires std::is_enum_v<E>
inline auto GetCategoryInstance() noexcept -> const ErrorCategory* {
    // Force compiler instantiation of EnumToString<E> via immediate invocation to prevent link-time undefined symbol errors in Clang
    [[maybe_unused]] auto dummy = Reflect::EnumToString(E {});

    static constexpr ErrorCategory cat = {
        .name      = Reflect::TypeName<E>(),
        .to_string = [](uint32_t val) noexcept -> std::string_view {
            // Using abstracted EnumToMessage to fetch annotations, falling back to string names
            return Reflect::EnumToMessage(static_cast<E>(val));
        },
        .to_name = [](uint32_t val) noexcept -> std::string_view {
            // The bare enumerator identifier: summaries report which enum VALUE an error is,
            // while to_string may return prose from the enumerator's Description annotation.
            return Reflect::EnumToString(static_cast<E>(val));
        }
    };
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

} // namespace TemplatedDetail

// ============================================================================
// The 8-Byte Error Carrier
// ============================================================================

struct ErrorCode {
    uint32_t category = 0;
    uint32_t value    = 0;

    constexpr ErrorCode() noexcept = default;
    constexpr ErrorCode(uint32_t cat, uint32_t val) noexcept: category(cat), value(val) {
    }

    /// The only templated entry point: E's type name hashes into the category
    /// word, the enumerator itself becomes the value word.
    template <typename E>
        requires std::is_enum_v<E>
    constexpr ErrorCode(E val) noexcept: category(Hash32(Reflect::TypeName<E>())), value(static_cast<uint32_t>(val)) {
        // Auto-registration: the carrier's one side effect, and the only E-typed
        // place it can happen (see this header's preamble). It is what lets a
        // later promotion to Error name the category and print the annotated
        // message instead of "None"; the enum is otherwise unknowable from a
        // bare {category, value} pair. Constant evaluation skips it -- there is
        // no process-wide registry to populate at compile time.
        if consteval {
        } else {
            [[maybe_unused]] bool dummy = TemplatedDetail::CategoryRegistration<E>::registered;
        }
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value != 0;
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr bool Is() const noexcept {
        return category == Hash32(Reflect::TypeName<E>());
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr bool Is(E val) const noexcept {
        return Is<E>() && value == static_cast<uint32_t>(val);
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr E As() const noexcept {
        return static_cast<E>(value);
    }

    constexpr auto operator==(const ErrorCode& other) const noexcept -> bool = default;

    /// Convert to the rich Error only where diagnostics/strings are needed.
    [[nodiscard]] Error ToError() const noexcept;
};

static_assert(sizeof(ErrorCode) == 8);
static_assert(std::is_standard_layout_v<ErrorCode> && std::is_trivially_copyable_v<ErrorCode>);

} // namespace ZHLN
