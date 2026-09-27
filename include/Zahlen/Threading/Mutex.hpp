// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
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
    alignas(16) ZHLN::Atomic<bool>      hasOwner;
    alignas(16) ZHLN::Atomic<uintptr_t> owner;

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

    ZHLN::Atomic<uint8_t> _bits;

    [[no_unique_address]] Owner _owner;

    [[gnu::cold, gnu::noinline]] void LockSlow() noexcept;
    [[gnu::cold, gnu::noinline]] void UnlockSlow() noexcept;

    void CheckPreLock() noexcept;
    void PostLock() noexcept;
    void PreUnlock() noexcept;
    void ClearOwner() noexcept;
};

static_assert(isDebug || sizeof(Mutex) == 1, "ZHLN::Mutex must be exactly 1 byte in Release mode!");

static_assert(
    isDebug || (std::is_trivially_default_constructible_v<Mutex> && std::is_standard_layout_v<Mutex> && std::is_trivially_copyable_v<Mutex>),
    "Mutex must remain a trivial C-compatible byte in Release mode!"
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
