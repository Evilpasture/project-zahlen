// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Mutex.hpp"
#include <detail/Atomic.hpp>
#include <type_traits>

namespace ZHLN {

/**
 * @class ConditionalVariable
 * @brief A memory-efficient 1-byte Condition Variable compatible with both OS threads and Fibers.
 *
 * Leverages the same sharded parking lot backend as ZHLN::Mutex to eliminate the "thundering herd"
 * problem and keep a near-zero memory footprint.
 */
class ConditionalVariable {
  public:
    // No custom or defaulted constructors are declared here.
    // This allows the compiler to generate the implicit trivial default constructor.

    /**
     * @brief Releases the mutex and blocks until notified.
     */
    void Wait(Mutex& mutex) noexcept;

    /**
     * @brief Unblocks one waiting thread or fiber.
     */
    void NotifyOne() noexcept;

    /**
     * @brief Unblocks all waiting threads and fibers.
     */
    void NotifyAll() noexcept;

  private:
    // Trivial uninitialized atomic state to guarantee standard-layout / triviality.
    // 0 = No active waiters, 1 = Active waiters present
    ZHLN::Atomic<uint8_t> _bits;
};

static_assert(sizeof(ConditionalVariable) == 1, "ZHLN::ConditionalVariable must be exactly 1 byte!");
static_assert(std::is_standard_layout_v<ConditionalVariable>, "ZHLN::ConditionalVariable must be standard layout!");
static_assert(std::is_trivially_default_constructible_v<ConditionalVariable>, "ZHLN::ConditionalVariable must be trivially default constructible!");
static_assert(std::is_trivially_copyable_v<ConditionalVariable>, "ZHLN::ConditionalVariable must be trivially copyable!");

} // namespace ZHLN
