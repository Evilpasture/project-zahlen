// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Mutex.hpp"
#include <detail/Atomic.hpp>
#include <mutex> // Added for std::unique_lock
#include <type_traits>

namespace ZHLN {

/**
 * @class ConditionalVariable
 * @brief A memory-efficient 1-byte Condition Variable compatible with both OS threads and Fibers.
 */
class ConditionalVariable {
  public:
    constexpr ConditionalVariable() noexcept = default;
    ~ConditionalVariable()                   = default;

    ConditionalVariable(const ConditionalVariable&)            = delete;
    ConditionalVariable& operator=(const ConditionalVariable&) = delete;

    /**
     * @brief Releases the mutex and blocks until notified.
     */
    void wait(Mutex& mutex) noexcept;

    /**
     * @brief Overload that accepts std::unique_lock<Mutex> directly to match STL patterns.
     */
    void wait(std::unique_lock<Mutex>& lock) noexcept;

    /**
     * @brief Releases the mutex and blocks until notified, repeating until the predicate is satisfied.
     * Mimics the STL std::condition_variable::wait overload to handle spurious wakeups.
     */
    template <typename Predicate>
    void wait(Mutex& mutex, Predicate pred) {
        while (!pred()) {
            wait(mutex);
        }
    }

    /**
     * @brief Releases the unique_lock and blocks until notified, repeating until the predicate is satisfied.
     */
    template <typename Predicate>
    void wait(std::unique_lock<Mutex>& lock, Predicate pred) {
        while (!pred()) {
            wait(lock);
        }
    }

    /**
     * @brief Unblocks one waiting thread or fiber.
     */
    void notify_one() noexcept;

    /**
     * @brief Unblocks all waiting threads and fibers.
     */
    void notify_all() noexcept;

  private:
    ZHLN::Atomic<uint8_t> _bits;
};

static_assert(sizeof(ConditionalVariable) == 1, "ZHLN::ConditionalVariable must be exactly 1 byte!");
static_assert(std::is_standard_layout_v<ConditionalVariable>, "ZHLN::ConditionalVariable must be standard layout!");
static_assert(std::is_trivially_default_constructible_v<ConditionalVariable>, "ZHLN::ConditionalVariable must be trivially default constructible!");
static_assert(std::is_trivially_copyable_v<ConditionalVariable>, "ZHLN::ConditionalVariable must be trivially copyable!");

} // namespace ZHLN
