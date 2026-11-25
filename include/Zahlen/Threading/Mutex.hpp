// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace ZHLN {

struct Fiber;
extern auto GetCurrentFiber() noexcept -> Fiber*;
extern void YieldFiber() noexcept;

namespace detail {

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

}

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

    ZHLN::Atomic<uint8_t> _bits {UNLOCKED};

    [[no_unique_address]] Owner _owner {};

    // Reached from the inline Lock()/Unlock() fast paths, so these cannot be
    // hidden: every consumer's own objects call them. Exported for that reason
    // even though no caller ever names them.
    [[gnu::cold, gnu::noinline]] ZHLN_API void LockSlow() noexcept;
    [[gnu::cold, gnu::noinline]] ZHLN_API void UnlockSlow() noexcept;

    // Same argument as LockSlow/UnlockSlow: the inline fast paths call these
    // when isDebug, and that code is instantiated in every consumer DSO. The
    // release build never references them (isDebug is false), which is exactly
    // why hiding them went unnoticed until someone built -O0/Debug.
    ZHLN_API void CheckPreLock() noexcept;
    ZHLN_API void PostLock() noexcept;
    ZHLN_API void PreUnlock() noexcept;
    ZHLN_API void ClearOwner() noexcept;
};

static_assert(isDebug || sizeof(Mutex) == 1, "ZHLN::Mutex must be exactly 1 byte in Release mode!");

static_assert(
    isDebug || (std::is_nothrow_default_constructible_v<Mutex> && std::is_trivially_destructible_v<Mutex> &&
                std::is_standard_layout_v<Mutex> && std::is_trivially_copyable_v<Mutex>),
    "Mutex must remain a 1-byte standard-layout, trivially-copyable lock initialized to UNLOCKED in Release mode!"
);

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

template <typename MutexT, typename Func>
decltype(auto) Lock(MutexT& mutex, Func&& func) {
    MutexGuard guard(mutex);
    return std::forward<Func>(func)();
}

}
