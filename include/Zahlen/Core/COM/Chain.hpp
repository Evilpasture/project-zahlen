// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Core/COM/Chain.hpp
//
// Non-intrusive reflected sType/pNext chains for ABI-stable configuration,
// creation descriptors and partial state queries across the COM boundary.
//
// Plain C++ value structs stay free of sType and pNext fields (and free of base
// classes, which would forfeit standard layout). Instead, ChainInNode and
// ChainOutNode sit on the caller's stack, stamp each payload's canonical type
// hash, recursive leaf LayoutHash and byte size at consteval time, and link
// themselves into a singly linked list.
//
// On the callee side, DispatchInChain / DispatchOutChain walk pNext, reject
// unknown structure types, duplicate structure types, null payloads, and
// size/layout drift with explicit ComError codes rather than reading past the
// caller's allocation.

#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Reflection/CanonicalName.hpp>
#include <Zahlen/Core/Reflection/LayoutHash.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN::COM {

struct ChainInNode {
    uint64_t           sType      = 0;
    uint64_t           layoutHash = 0;
    uint32_t           size       = 0;
    uint32_t           reserved   = 0;
    const void*        data       = nullptr;
    const ChainInNode* pNext      = nullptr;
};

struct ChainOutNode {
    uint64_t      sType      = 0;
    uint64_t      layoutHash = 0;
    uint32_t      size       = 0;
    uint32_t      reserved   = 0;
    void*         data       = nullptr;
    ChainOutNode* pNext      = nullptr;
};

static_assert(std::is_standard_layout_v<ChainInNode> && std::is_trivially_copyable_v<ChainInNode>);
static_assert(std::is_standard_layout_v<ChainOutNode> && std::is_trivially_copyable_v<ChainOutNode>);

template <typename T>
[[nodiscard]] consteval auto ChainTypeID() noexcept -> uint64_t {
    using Bare = std::remove_cvref_t<T>;
    return Hash64(Reflect::CanonicalTypeName<Bare>());
}

template <typename T>
concept ChainPayload =
    !std::is_pointer_v<std::remove_cvref_t<T>> &&
    !std::is_same_v<std::remove_cvref_t<T>, ChainInNode> &&
    !std::is_same_v<std::remove_cvref_t<T>, ChainOutNode> &&
    std::is_standard_layout_v<std::remove_cvref_t<T>> &&
    std::is_trivially_copyable_v<std::remove_cvref_t<T>>;

namespace TemplatedDetail {

inline constexpr std::size_t kMaxChainDepth = 64;

template <typename... Ts>
[[nodiscard]] consteval auto ChainTypesUnique() noexcept -> bool {
    if constexpr (sizeof...(Ts) <= 1) {
        return true;
    } else {
        constexpr std::array<uint64_t, sizeof...(Ts)> ids {ChainTypeID<Ts>()...};
        for (std::size_t i = 0; i < ids.size(); ++i) {
            for (std::size_t j = i + 1; j < ids.size(); ++j) {
                if (ids[i] == ids[j]) return false;
            }
        }
        return true;
    }
}

// Lvalue arguments bind by const reference (zero copy of large structs like
// GraphicsSettings); rvalue temporaries are stored by value inside the pack so
// `const auto chain = COM::InChain(MyConfig{...});` never dangles.
template <typename Arg>
using StoredChainArg = std::conditional_t<
    std::is_lvalue_reference_v<Arg>,
    const std::remove_cvref_t<Arg>&,
    std::remove_cvref_t<Arg>>;

struct EmptyChainIn {
    [[nodiscard]] constexpr auto Head() const noexcept -> const ChainInNode* {
        return nullptr;
    }

    [[nodiscard]] constexpr operator const ChainInNode*() const noexcept {
        return nullptr;
    }
};

template <typename... Args>
class ChainInPack {
    static_assert(sizeof...(Args) > 0);
    static_assert((ChainPayload<Args> && ...), "Every COM::InChain element must be a fixed-layout value struct.");
    static_assert(ChainTypesUnique<std::remove_cvref_t<Args>...>(),
                  "A COM::InChain may not contain two nodes of the same structure type.");

public:
    explicit constexpr ChainInPack(const ChainInNode* tail, Args&&... args) noexcept
        : m_payloads(std::forward<Args>(args)...) {
        InitNodes(tail, std::index_sequence_for<Args...> {});
    }

    ChainInPack(const ChainInPack&)            = delete;
    ChainInPack& operator=(const ChainInPack&) = delete;
    ChainInPack(ChainInPack&&)                 = delete;
    ChainInPack& operator=(ChainInPack&&)      = delete;

    [[nodiscard]] constexpr auto Head() const noexcept -> const ChainInNode* {
        return &m_nodes[0];
    }

    [[nodiscard]] constexpr operator const ChainInNode*() const noexcept {
        return &m_nodes[0];
    }

private:
    template <std::size_t... Is>
    constexpr void InitNodes(const ChainInNode* tail, std::index_sequence<Is...>) noexcept {
        constexpr std::size_t kCount = sizeof...(Args);
        ((m_nodes[Is] = ChainInNode {
              .sType      = ChainTypeID<std::tuple_element_t<Is, std::tuple<std::remove_cvref_t<Args>...>>>(),
              .layoutHash = Reflect::LayoutHash<std::tuple_element_t<Is, std::tuple<std::remove_cvref_t<Args>...>>>(),
              .size       = static_cast<uint32_t>(sizeof(std::tuple_element_t<Is, std::tuple<std::remove_cvref_t<Args>...>>)),
              .reserved   = 0,
              .data       = &std::get<Is>(m_payloads),
              .pNext      = (Is + 1 < kCount) ? &m_nodes[Is + 1] : tail,
          }),
         ...);
    }

    std::tuple<StoredChainArg<Args>...>      m_payloads;
    std::array<ChainInNode, sizeof...(Args)> m_nodes {};
};

template <typename... Ts>
class ChainOutPack {
    static_assert(sizeof...(Ts) > 0, "COM::OutChain requires at least one output target.");
    static_assert((!std::is_const_v<Ts> && ...), "COM::OutChain targets must be mutable lvalues.");
    static_assert((ChainPayload<Ts> && ...), "Every COM::OutChain element must be a fixed-layout value struct.");
    static_assert(ChainTypesUnique<Ts...>(), "A COM::OutChain may not contain two nodes of the same structure type.");

public:
    explicit constexpr ChainOutPack(Ts&... targets) noexcept {
        InitNodes(std::index_sequence_for<Ts...> {}, targets...);
    }

    ChainOutPack(const ChainOutPack&)            = delete;
    ChainOutPack& operator=(const ChainOutPack&) = delete;
    ChainOutPack(ChainOutPack&&)                 = delete;
    ChainOutPack& operator=(ChainOutPack&&)      = delete;

    [[nodiscard]] constexpr auto Head() const noexcept -> ChainOutNode* {
        return &m_nodes[0];
    }

    [[nodiscard]] constexpr operator ChainOutNode*() const noexcept {
        return &m_nodes[0];
    }

private:
    template <std::size_t... Is>
    constexpr void InitNodes(std::index_sequence<Is...>, Ts&... targets) noexcept {
        constexpr std::size_t kCount = sizeof...(Ts);
        auto                  ptrs   = std::tuple<Ts*...> {&targets...};
        ((m_nodes[Is] = ChainOutNode {
              .sType      = ChainTypeID<Ts>(),
              .layoutHash = Reflect::LayoutHash<Ts>(),
              .size       = static_cast<uint32_t>(sizeof(Ts)),
              .reserved   = 0,
              .data       = std::get<Is>(ptrs),
              .pNext      = (Is + 1 < kCount) ? &m_nodes[Is + 1] : nullptr,
          }),
         ...);
    }

    mutable std::array<ChainOutNode, sizeof...(Ts)> m_nodes {};
};

template <typename Allowed, std::size_t Index, std::size_t Total, typename Visitor>
[[nodiscard]] constexpr auto TryMatchInNode(const ChainInNode& node, std::array<bool, Total>& seen, bool& matched,
                                            Visitor& visitor) noexcept -> Result<void> {
    if (matched || node.sType != ChainTypeID<Allowed>()) {
        return {};
    }
    matched = true;
    if (seen[Index]) {
        return ComError::DuplicateExtension;
    }
    seen[Index] = true;
    if (node.size != sizeof(Allowed) || node.layoutHash != Reflect::LayoutHash<Allowed>()) {
        return ComError::LayoutMismatch;
    }
    const auto& typed = *static_cast<const Allowed*>(node.data);
    if constexpr (std::is_void_v<decltype(visitor(typed))>) {
        visitor(typed);
        return {};
    } else {
        return visitor(typed);
    }
}

template <typename Allowed, std::size_t Index, std::size_t Total, typename Visitor>
[[nodiscard]] constexpr auto TryMatchOutNode(ChainOutNode& node, std::array<bool, Total>& seen, bool& matched,
                                             Visitor& visitor) noexcept -> Result<void> {
    if (matched || node.sType != ChainTypeID<Allowed>()) {
        return {};
    }
    matched = true;
    if (seen[Index]) {
        return ComError::DuplicateExtension;
    }
    seen[Index] = true;
    if (node.size != sizeof(Allowed) || node.layoutHash != Reflect::LayoutHash<Allowed>()) {
        return ComError::LayoutMismatch;
    }
    auto& typed = *static_cast<Allowed*>(node.data);
    if constexpr (std::is_void_v<decltype(visitor(typed))>) {
        visitor(typed);
        return {};
    } else {
        return visitor(typed);
    }
}

template <typename... AllowedTs, std::size_t... Is, typename Visitor>
[[nodiscard]] constexpr auto DispatchInNodeImpl(const ChainInNode& node, std::array<bool, sizeof...(AllowedTs)>& seen,
                                                Visitor& visitor, std::index_sequence<Is...>) noexcept -> Result<void> {
    bool         matched = false;
    Result<void> outcome {};
    ((matched ? void() : (outcome = TryMatchInNode<AllowedTs, Is, sizeof...(AllowedTs)>(node, seen, matched, visitor), void())), ...);
    if (!matched) {
        return ComError::UnsupportedExtension;
    }
    return outcome;
}

template <typename... AllowedTs, std::size_t... Is, typename Visitor>
[[nodiscard]] constexpr auto DispatchOutNodeImpl(ChainOutNode& node, std::array<bool, sizeof...(AllowedTs)>& seen,
                                                 Visitor& visitor, std::index_sequence<Is...>) noexcept -> Result<void> {
    bool         matched = false;
    Result<void> outcome {};
    ((matched ? void() : (outcome = TryMatchOutNode<AllowedTs, Is, sizeof...(AllowedTs)>(node, seen, matched, visitor), void())), ...);
    if (!matched) {
        return ComError::UnsupportedExtension;
    }
    return outcome;
}

} // namespace TemplatedDetail

[[nodiscard]] constexpr auto InChain() noexcept -> TemplatedDetail::EmptyChainIn {
    return {};
}

template <ChainPayload... Args>
    requires (sizeof...(Args) > 0)
[[nodiscard]] constexpr auto InChain(Args&&... args) noexcept -> TemplatedDetail::ChainInPack<Args...> {
    return TemplatedDetail::ChainInPack<Args...>(nullptr, std::forward<Args>(args)...);
}

template <ChainPayload... Args>
    requires (sizeof...(Args) > 0)
[[nodiscard]] constexpr auto InChainWithTail(const ChainInNode* tail, Args&&... args) noexcept
    -> TemplatedDetail::ChainInPack<Args...> {
    return TemplatedDetail::ChainInPack<Args...>(tail, std::forward<Args>(args)...);
}

template <ChainPayload... Ts>
    requires (sizeof...(Ts) > 0 && (!std::is_const_v<Ts> && ...))
[[nodiscard]] constexpr auto OutChain(Ts&... targets) noexcept -> TemplatedDetail::ChainOutPack<Ts...> {
    return TemplatedDetail::ChainOutPack<Ts...>(targets...);
}

template <ChainPayload... AllowedTs, typename Visitor>
    requires (sizeof...(AllowedTs) > 0)
[[nodiscard]] constexpr auto DispatchInChain(const ChainInNode* head, Visitor&& visitor) noexcept -> Result<void> {
    static_assert(TemplatedDetail::ChainTypesUnique<AllowedTs...>(),
                  "DispatchInChain allowed type list must not contain duplicates.");
    std::array<bool, sizeof...(AllowedTs)> seen {};
    std::size_t                            depth = 0;
    for (const ChainInNode* current = head; current != nullptr; current = current->pNext) {
        if (++depth > TemplatedDetail::kMaxChainDepth) {
            return ComError::DuplicateExtension;
        }
        if (current->data == nullptr) {
            return ComError::NullArgument;
        }
        const Result<void> step =
            TemplatedDetail::DispatchInNodeImpl<AllowedTs...>(*current, seen, visitor, std::index_sequence_for<AllowedTs...> {});
        if (!step.HasValue()) {
            return step;
        }
    }
    return {};
}

template <ChainPayload... AllowedTs, typename Visitor>
    requires (sizeof...(AllowedTs) > 0)
[[nodiscard]] constexpr auto DispatchOutChain(ChainOutNode* head, Visitor&& visitor) noexcept -> Result<void> {
    static_assert(TemplatedDetail::ChainTypesUnique<AllowedTs...>(),
                  "DispatchOutChain allowed type list must not contain duplicates.");
    if (head == nullptr) {
        return ComError::NullArgument;
    }
    std::array<bool, sizeof...(AllowedTs)> seen {};
    std::size_t                            depth = 0;
    for (ChainOutNode* current = head; current != nullptr; current = current->pNext) {
        if (++depth > TemplatedDetail::kMaxChainDepth) {
            return ComError::DuplicateExtension;
        }
        if (current->data == nullptr) {
            return ComError::NullArgument;
        }
        const Result<void> step =
            TemplatedDetail::DispatchOutNodeImpl<AllowedTs...>(*current, seen, visitor, std::index_sequence_for<AllowedTs...> {});
        if (!step.HasValue()) {
            return step;
        }
    }
    return {};
}

} // namespace ZHLN::COM
