// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


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


struct ErrorCategory {
    std::string_view name;
    std::string_view (*to_string)(uint32_t) noexcept;
    std::string_view (*to_name)(uint32_t) noexcept;
};

namespace TemplatedDetail {

constexpr auto HashTypeName(std::string_view str) noexcept -> uint32_t {
    return Hash32(str);
}

template <typename E>
    requires std::is_enum_v<E>
inline auto GetCategoryInstance() noexcept -> const ErrorCategory* {
    [[maybe_unused]] auto dummy = Reflect::EnumToString(E {});

    static constexpr ErrorCategory cat = {
        .name      = Reflect::TypeName<E>(),
        .to_string = [](uint32_t val) noexcept -> std::string_view {
            return Reflect::EnumToMessage(static_cast<E>(val));
        },
        .to_name = [](uint32_t val) noexcept -> std::string_view {
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

inline auto GetRegistryHead() noexcept -> std::atomic<RegistryNode*>& {
    static std::atomic<RegistryNode*> head {nullptr};
    return head;
}

template <typename E>
    requires std::is_enum_v<E>
struct CategoryRegistration {
    static inline RegistryNode node = {.hash = HashTypeName(ZHLN::Reflect::TypeName<E>()), .category = GetCategoryInstance<E>(), .next = nullptr};

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

}

inline void RegisterErrorCategory(const uint32_t hash, const ErrorCategory* category) noexcept {
    auto& head = TemplatedDetail::GetRegistryHead();
    for (auto* curr = head.load(std::memory_order::acquire); curr != nullptr; curr = curr->next) {
        if (curr->hash == hash) {
            return;
        }
    }
    auto* node     = new TemplatedDetail::RegistryNode {hash, category, head.load(std::memory_order::relaxed)};
    auto  expected = node->next;
    while (!head.compare_exchange_weak(expected, node, std::memory_order::release, std::memory_order::relaxed)) {
        node->next = expected;
    }
}

extern void ERROR_CODE_CANNOT_BE_ZERO();


struct ErrorCode {
    constexpr ErrorCode() noexcept = default;
    constexpr ErrorCode(uint32_t cat, uint32_t val) noexcept: category(cat), value(val) {
    }

    template <typename E>
        requires std::is_enum_v<E>
    constexpr ErrorCode(E val) noexcept: category(Hash32(Reflect::TypeName<E>())), value(static_cast<uint32_t>(val)) {
        static_assert(
            !Reflect::EnumHasValue<E>(0),
            "Error enums must not contain a 0 enumerator: ErrorCode's value word uses 0 for 'no "
            "error'. Start error enumerators at 1; wrap foreign codes (e.g. VkResult) in the "
            "boundary carrier (Vk::Error) instead."
        );
        if (static_cast<uint32_t>(val) == 0) {
            if consteval {
                ERROR_CODE_CANNOT_BE_ZERO();
            } else {
                DebugBreak();
            }
        }
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

    [[nodiscard]] Error ToError() const noexcept;

  private:
    uint32_t category = 0;
    uint32_t value    = 0;

    friend class Error;
};

static_assert(sizeof(ErrorCode) == 8);
static_assert(std::is_standard_layout_v<ErrorCode> && std::is_trivially_copyable_v<ErrorCode>);

}
