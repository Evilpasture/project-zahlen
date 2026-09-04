// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <cstdint>

namespace ZHLN {

// Forward declare the Fiber system
struct Fiber;
extern auto GetCurrentFiber() noexcept -> Fiber*;
extern void YieldFiber() noexcept;

/**
 * @brief High-Performance, 1-Byte Mutex.
 * Satisfies the C++ `BasicLockable` requirement (`std::lock_guard` compatible).
 */
class Mutex {
  public:
    constexpr Mutex() noexcept = default;
    ~Mutex()                   = default;

    [[gnu::flatten, gnu::hot, gnu::always_inline]]
    void lock() noexcept {
        if constexpr (isDebug) {
            CheckPreLock();
        }

        uint8_t expected = UNLOCKED;
        if (_bits.compare_exchange_strong(expected, LOCKED, std::memory_order::acquire, std::memory_order::relaxed)) [[likely]] {
            if constexpr (isDebug) {
                PostLock();
            }
            return;
        }
        LockSlow();
    }

    [[gnu::flatten, gnu::hot, gnu::always_inline]]
    void unlock() noexcept {
        if constexpr (isDebug) {
            PreUnlock();
            ClearOwner();
        }

        uint8_t expected = LOCKED;
        if (_bits.compare_exchange_strong(expected, UNLOCKED, std::memory_order::release, std::memory_order::relaxed)) [[likely]] {
            return;
        }
        UnlockSlow();
    }

    [[gnu::flatten, gnu::hot, gnu::always_inline]]
    auto try_lock() noexcept -> bool {
        if constexpr (isDebug) {
            CheckPreLock();
        }

        uint8_t expected = UNLOCKED;
        bool    success  = _bits.compare_exchange_strong(expected, LOCKED, std::memory_order::acquire, std::memory_order::relaxed);
        if constexpr (isDebug) {
            if (success) {
                PostLock();
            }
        }
        return success;
    }

  private:
    static constexpr uint8_t UNLOCKED    = 0x00;
    static constexpr uint8_t LOCKED      = 0x01;
    static constexpr uint8_t HAS_WAITERS = 0x02;
    static constexpr uint8_t POISONED    = 0x04;

    // Raw zero is the unlocked C/FFI representation. Keep this member free of
    // NSDMI so Mutex remains trivially default constructible; C++ owners must
    // value-initialize (`Mutex mutex {}`), and FFI storage must be zeroed.
    ZHLN::Atomic<uint8_t> _bits;

    [[gnu::cold, gnu::noinline]] void LockSlow() noexcept;
    [[gnu::cold, gnu::noinline]] void UnlockSlow() noexcept;

    // --- Debug Variables & Helpers ---
#ifdef ZHLN_DEBUG
    alignas(16) ZHLN::Atomic<bool> _hasOwner {};
    alignas(16) ZHLN::Atomic<uintptr_t> _owner {};

    void CheckPreLock() noexcept;
    void PostLock() noexcept;
    void PreUnlock() noexcept;
    void ClearOwner() noexcept;
#else
    constexpr void CheckPreLock() noexcept {
    }
    constexpr void PostLock() noexcept {
    }
    constexpr void PreUnlock() noexcept {
    }
    constexpr void ClearOwner() noexcept {
    }
#endif
};

// Guarantee 1-byte footprint in Release builds
static_assert(isDebug || sizeof(Mutex) == 1, "ZHLN::Mutex must be exactly 1 byte in Release mode!");

static_assert(
    isDebug || (std::is_trivially_default_constructible_v<Mutex> && std::is_standard_layout_v<Mutex> && std::is_trivially_copyable_v<Mutex>),
    "Mutex must remain a trivial C-compatible byte in Release mode!"
);

/**
 * @brief Trivial RAII guard to avoid including <mutex> in interface headers.
 */
struct MutexGuard {
    Mutex& _m;
    explicit MutexGuard(Mutex& m) noexcept: _m(m) {
        _m.lock();
    }
    ~MutexGuard() noexcept {
        _m.unlock();
    }

    MutexGuard(const MutexGuard&)                    = delete;
    auto operator=(const MutexGuard&) -> MutexGuard& = delete;
};

} // namespace ZHLN
