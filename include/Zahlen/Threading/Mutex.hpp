// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Atomic.hpp>
#include <cstdint>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace ZHLN {

// Forward declare the Fiber system
struct Fiber;
extern Fiber* GetCurrentFiber() noexcept;
extern void   YieldFiber() noexcept;

inline constexpr bool kIsDebugMutex =
#ifdef ZHLN_DEBUG
    true;
#else
    false;
#endif

/**
 * @brief High-Performance, 1-Byte Mutex.
 * Satisfies the C++ `BasicLockable` requirement (`std::lock_guard` compatible).
 */
class Mutex {
  public:
    constexpr Mutex() noexcept = default;
    ~Mutex()                   = default;

    [[gnu::flatten, gnu::hot, gnu::always_inline]]
    inline void lock() noexcept {
        if constexpr (kIsDebugMutex) {
            CheckPreLock();
        }

        uint8_t expected = UNLOCKED;
        if (_bits.compare_exchange_strong(expected, LOCKED, std::memory_order::acquire, std::memory_order::relaxed)) [[likely]] {
            if constexpr (kIsDebugMutex) {
                PostLock();
            }
            return;
        }
        LockSlow();
    }

    [[gnu::flatten, gnu::hot, gnu::always_inline]]
    inline void unlock() noexcept {
        if constexpr (kIsDebugMutex) {
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
    inline bool try_lock() noexcept {
        if constexpr (kIsDebugMutex) {
            CheckPreLock();
        }

        uint8_t expected = UNLOCKED;
        bool    success  = _bits.compare_exchange_strong(expected, LOCKED, std::memory_order::acquire, std::memory_order::relaxed);
        if constexpr (kIsDebugMutex) {
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

    ZHLN::Atomic<uint8_t> _bits {};

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
static_assert(kIsDebugMutex || sizeof(Mutex) == 1, "ZHLN::Mutex must be exactly 1 byte in Release mode!");

// Initialization is explicit (an indeterminate lock byte is not a valid mutex),
// while the type remains standard-layout and trivially copyable for dense ECS use.
static_assert(kIsDebugMutex || (std::is_standard_layout_v<Mutex> && std::is_trivially_copyable_v<Mutex>), "Mutex must remain POD-like in Release mode!");

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

    MutexGuard(const MutexGuard&)            = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;
};

inline void CPURelax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

} // namespace ZHLN
