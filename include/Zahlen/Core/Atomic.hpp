// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <atomic>
#include <concepts>
#include <type_traits>

namespace ZHLN {

template <typename T>
concept AtomicScalar = std::is_scalar_v<T> && std::is_standard_layout_v<T>;

/**
 * @brief Strictly Trivial (POD) Atomic Wrapper.
 * Uses C++20 std::atomic_ref to operate on raw memory safely.
 */
template <AtomicScalar T>
struct Atomic {
    using m_order = std::memory_order;
    using a_ref   = std::atomic_ref<T>;
    alignas(a_ref::required_alignment) mutable T value;

    [[gnu::always_inline]]
    void store(T desired, m_order order = m_order::seq_cst) noexcept {
        a_ref(value).store(desired, order);
    }

    [[nodiscard, gnu::always_inline]]
    auto load(std::memory_order order = std::memory_order::seq_cst) const noexcept -> T {
        return a_ref(value).load(order);
    }

    [[nodiscard, gnu::always_inline]]
    auto exchange(T desired, m_order order = m_order::seq_cst) noexcept -> T {
        return a_ref(value).exchange(desired, order);
    }

    [[gnu::always_inline]]
    auto compare_exchange_weak(T& expected, T desired, m_order success = m_order::seq_cst, m_order failure = m_order::seq_cst) noexcept -> bool {
        return a_ref(value).compare_exchange_weak(expected, desired, success, failure);
    }

    [[gnu::always_inline]]
    auto compare_exchange_strong(T& expected, T desired, m_order success = m_order::seq_cst, m_order failure = m_order::seq_cst) noexcept -> bool {
        return a_ref(value).compare_exchange_strong(expected, desired, success, failure);
    }

    // --- Arithmetic Operators (Constrained to Integral / Pointers) ---

    [[gnu::always_inline]]
    auto fetch_add(T arg, m_order order = m_order::seq_cst) noexcept -> T
        requires std::is_integral_v<T> || std::is_pointer_v<T>
    {
        return a_ref(value).fetch_add(arg, order);
    }

    [[gnu::always_inline]]
    auto fetch_sub(T arg, m_order order = m_order::seq_cst) noexcept -> T
        requires std::is_integral_v<T> || std::is_pointer_v<T>
    {
        return a_ref(value).fetch_sub(arg, order);
    }

    // --- Bitwise Operators (Strictly Constrained to Integral Types) ---

    [[gnu::always_inline]]
    auto fetch_and(T arg, m_order order = m_order::seq_cst) noexcept -> T
        requires std::integral<T>
    {
        return a_ref(value).fetch_and(arg, order);
    }

    [[gnu::always_inline]]
    auto fetch_or(T arg, m_order order = m_order::seq_cst) noexcept -> T
        requires std::integral<T>
    {
        return a_ref(value).fetch_or(arg, order);
    }

    [[gnu::always_inline]]
    auto fetch_xor(T arg, m_order order = m_order::seq_cst) noexcept -> T
        requires std::integral<T>
    {
        return a_ref(value).fetch_xor(arg, order);
    }

    [[gnu::always_inline]] auto operator++() noexcept -> T
        requires std::is_integral_v<T>
    {
        return fetch_add(1) + 1;
    }

    [[gnu::always_inline]] auto operator++(int) noexcept -> T
        requires std::is_integral_v<T>
    {
        return fetch_add(1);
    }

    // NOLINTNEXTLINE(cppcoreguidelines-c-copy-assignment-signature, misc-unconventional-assign-operator)
    [[gnu::always_inline]]
    auto operator=(T desired) noexcept -> T {
        store(desired);
        return desired;
    }

    [[gnu::always_inline]] operator T() const noexcept {
        return load();
    }
};

static_assert((std::is_trivially_default_constructible_v<Atomic<size_t>> && std::is_trivially_copyable_v<Atomic<size_t>>), "ZHLN::Atomic must be Trivial");

} // namespace ZHLN
