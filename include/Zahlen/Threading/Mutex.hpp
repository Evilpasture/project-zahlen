// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <cstdint>
#include <type_traits>

namespace ZHLN {

// Forward declare the Fiber system
struct Fiber;
extern auto GetCurrentFiber() noexcept -> Fiber*;
extern void YieldFiber() noexcept;

namespace detail {

/**
 * @brief Remembers which context owns a mutex, so that Mutex.cpp can catch
 * recursive locking and unlocking a mutex someone else holds.
 *
 * Both variants answer the same questions. `NoMutexOwner` is the release variant and
 * carries no state at all, which is what keeps Mutex a single C-compatible byte
 * -- see the static_asserts below. The shared shape is also what lets the
 * checks in Mutex.cpp be written once and guarded by `if constexpr (isDebug)`
 * instead of being preprocessed away: the discarded branch still has to
 * type-check, so the calls it makes have to exist in both configurations.
 */
struct MutexOwner {
    alignas(16) ZHLN::Atomic<bool>      hasOwner {false};
    alignas(16) ZHLN::Atomic<uintptr_t> owner {0};

    [[nodiscard]] auto IsOwned() const noexcept -> bool {
        return hasOwner.load(std::memory_order::acquire);
    }

    [[nodiscard]] auto OwnedBy(uintptr_t context) const noexcept -> bool {
        return hasOwner.load(std::memory_order::acquire) && owner.load(std::memory_order::relaxed) == context;
    }

    void SetOwner(uintptr_t context) noexcept {
        owner.store(context, std::memory_order::relaxed);
        hasOwner.store(true, std::memory_order::release);
    }

    void Reset() noexcept {
        hasOwner.store(false, std::memory_order::release);
    }
};

struct NoMutexOwner {
    [[nodiscard]] constexpr auto IsOwned() const noexcept -> bool {
        return false;
    }

    [[nodiscard]] constexpr auto OwnedBy(uintptr_t) const noexcept -> bool {
        return false;
    }

    constexpr void SetOwner(uintptr_t) noexcept {
    }

    constexpr void Reset() noexcept {
    }
};

} // namespace detail

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

    using Owner = std::conditional_t<isDebug, detail::MutexOwner, detail::NoMutexOwner>;

    // Raw zero is the unlocked C/FFI representation. Keep this member free of
    // NSDMI so Mutex remains trivially default constructible; C++ owners must
    // value-initialize (`Mutex mutex {}`), and FFI storage must be zeroed.
    ZHLN::Atomic<uint8_t> _bits;

    // Ownership bookkeeping for the deadlock detector: two atomics in a debug
    // build, an empty type in a release one, where [[no_unique_address]] lets it
    // take up no space at all. Deliberately no NSDMI, for the same reason as
    // _bits -- MutexOwner initializes its own members.
    [[no_unique_address]] Owner _owner;

    [[gnu::cold, gnu::noinline]] void LockSlow() noexcept;
    [[gnu::cold, gnu::noinline]] void UnlockSlow() noexcept;

    // --- Debug Hooks ---
    // Mutex.cpp defines these for both configurations and guards the bodies
    // with `if constexpr (isDebug)`, so a release build keeps the detector out
    // of the binary without a second, empty copy of every check living here.
    void CheckPreLock() noexcept;
    void PostLock() noexcept;
    void PreUnlock() noexcept;
    void ClearOwner() noexcept;
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
