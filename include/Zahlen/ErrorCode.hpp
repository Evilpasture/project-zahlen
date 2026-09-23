// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/ErrorCode.hpp
//
// The engine's error *channel*: everything fallible returns std::expected<T,
// ZHLN::ErrorCode> -- a category hash and an enumerator value, two uint32_t and nothing
// else. Constructing one from an enum costs a compile-time type-name hash and the register
// pair it lives in: no FormatConst expansion, no enumerator scan, no DebugBreak, no
// per-call reflection work. ZHLN::Error (Zahlen/Error.hpp) is the diagnostic form of the
// same two words, resolving category and message lazily, so promote only where text is
// wanted:
//
//     std::expected<Mesh, ErrorCode> Build();          // plumbing: the carrier
//     if (auto mesh = Build(); !mesh) {
//         const Error err = mesh.error();              // boundary: the rich form
//         Log("mesh build failed: {} ({})", err.Message(), err.Name());
//     }
//
// Both directions are implicit, and ErrorCode::ToError() spells the promotion out where a
// signature should say it.
//
// This header also owns the process-wide category registry, keyed by the hash of the enum's
// type name, which is what turns a {category, value} pair back into text later. An
// ErrorCode's enum constructor is the only place it *can* be populated for a given E -- the
// registry is addressed by hash, so by the time anyone asks for Message() the type is long
// gone. That touch is the whole cost the hot path pays: one relaxed load of an inline static
// bool per conversion, plus one lock-free push the first time each enum type is converted. A
// TU including only this header never instantiates ZHLN::Error.

#pragma once

#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace ZHLN {

class Error;

// Category Registry (shared by ErrorCode and Error)

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

// Non-constexpr undefined symbol hook: calling this during constant evaluation forces an immediate compile error
extern void ERROR_CODE_CANNOT_BE_ZERO();

// The 8-Byte Error Carrier

struct ErrorCode {
    constexpr ErrorCode() noexcept = default;
    constexpr ErrorCode(uint32_t cat, uint32_t val) noexcept: category(cat), value(val) {
    }

    // The only templated entry point: E's type name hashes into the category
    // word, the enumerator itself becomes the value word.
    //
    // It is also the channel's one enforcement point, because it is the only
    // way an enum enters it: the type must not contain a 0 enumerator
    // (rejected at compile time below), and the value must not be 0 (breaks
    // at run time below). Both exist for the same reason -- value 0 means "no
    // error": it is what ErrorCode{} carries and what operator bool tests, so
    // a zero-valued error would be indistinguishable from success. Foreign
    // codes with a zero enumerator (notably VkResult's VK_SUCCESS) cannot
    // cross here and must be mapped into an engine enum at the layer boundary
    // instead (see Vk::VulkanResult and Vk::ToFrameError).
    template <typename E>
        requires std::is_enum_v<E>
    constexpr ErrorCode(E val) noexcept: category(Hash32(Reflect::TypeName<E>())), value(static_cast<uint32_t>(val)) {
        static_assert(
            !Reflect::EnumHasValue<E>(0),
            "Error enums must not contain a 0 enumerator: ErrorCode's value word uses 0 for 'no "
            "error'. Start error enumerators at 1; map foreign codes (e.g. VkResult) into an engine "
            "enum at the layer boundary."
        );
        if (static_cast<uint32_t>(val) == 0) {
            if consteval {
                // Halts compilation immediately if a 0-valued error is created at compile time
                ERROR_CODE_CANNOT_BE_ZERO();
            } else {
                // Immediate crash if a 0 was dynamically converted to E at runtime
                DebugBreak();
            }
        }
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

    // Convert to the rich Error only where diagnostics/strings are needed.
    [[nodiscard]] Error ToError() const noexcept;

  private:
    // The two words are private, and that is the whole of this type's safety: an
    // error is a diagnostic, and an ordinal is meaningless without the category
    // that names its enum. With no public member and no conversion to an
    // integral type, `static_cast<int>(code)` has nothing to bite on. The
    // explicit operator bool above is not a way in either: an explicit
    // conversion function only feeds a direct-init when the standard conversion
    // following it is Exact Match, and bool -> int is a promotion. So
    // static_cast<int>, static_cast<uint32_t>, C-style (int)code and `int n =
    // code` are all ill-formed -- tests/core/TestError.cpp pins that with
    // static_asserts, and configure/check_error_ordinals.py rejects the one cast
    // that does survive, `static_cast<uint32_t>(err.As<E>())`, everywhere but the
    // scripting ABI that needs the ordinal on purpose.
    //
    // Reading the words is the promotion's job -- Error.hpp's constructor is the
    // single boundary that turns them back into text, which is why Error is the
    // only friend. A caller-facing path is Message()/Name()/Category(), the
    // formatter that wraps them, or Is<E>()/As<E>() for a category check.
    uint32_t category = 0;
    uint32_t value    = 0;

    friend class Error;
};

static_assert(sizeof(ErrorCode) == 8);
static_assert(std::is_standard_layout_v<ErrorCode> && std::is_trivially_copyable_v<ErrorCode>);

} // namespace ZHLN
