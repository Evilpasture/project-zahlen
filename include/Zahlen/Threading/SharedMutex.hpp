// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace ZHLN {

class SharedMutex {
  public:
    constexpr SharedMutex() noexcept = default;
    ~SharedMutex()                   = default;

    SharedMutex(const SharedMutex&)                    = delete;
    auto operator=(const SharedMutex&) -> SharedMutex& = delete;

    [[gnu::flatten, gnu::hot, gnu::always_inline]]
    void lock() noexcept {
        uint32_t expected = 0;
        if (_state.compare_exchange_strong(expected, WRITER_ACTIVE, std::memory_order::acquire, std::memory_order::relaxed)) [[likely]] {
            return;
        }
        LockSlow();
    }

    [[gnu::flatten, gnu::hot, gnu::always_inline]]
    void unlock() noexcept {
        uint32_t expected = WRITER_ACTIVE;
        if (_state.compare_exchange_strong(expected, 0, std::memory_order::release, std::memory_order::relaxed)) [[likely]] {
            return;
        }
        UnlockSlow();
    }

    [[gnu::flatten, gnu::hot, gnu::always_inline]]
    auto try_lock() noexcept -> bool {
        uint32_t expected = 0;
        return _state.compare_exchange_strong(expected, WRITER_ACTIVE, std::memory_order::acquire, std::memory_order::relaxed);
    }

    [[gnu::flatten, gnu::hot, gnu::always_inline]]
    void lock_shared() noexcept {
        uint32_t state = _state.load(std::memory_order::relaxed);
        if ((state & (WRITER_ACTIVE | WRITER_PENDING)) == 0) [[likely]] {
            if (_state.compare_exchange_weak(state, state + READER_UNIT, std::memory_order::acquire, std::memory_order::relaxed)) [[likely]] {
                return;
            }
        }
        LockSharedSlow();
    }

    [[gnu::flatten, gnu::hot, gnu::always_inline]]
    void unlock_shared() noexcept {
        const uint32_t prev = _state.fetch_sub(READER_UNIT, std::memory_order::release);
        if (((prev - READER_UNIT) >> READER_SHIFT) == 0 && (prev & WRITER_PARKED) != 0) [[unlikely]] {
            UnlockSharedSlow();
        }
    }

    [[gnu::flatten, gnu::hot, gnu::always_inline]]
    auto try_lock_shared() noexcept -> bool {
        uint32_t state = _state.load(std::memory_order::relaxed);
        while ((state & (WRITER_ACTIVE | WRITER_PENDING)) == 0) {
            if (_state.compare_exchange_weak(state, state + READER_UNIT, std::memory_order::acquire, std::memory_order::relaxed)) {
                return true;
            }
        }
        return false;
    }

  private:
    static constexpr uint32_t WRITER_ACTIVE  = 1u << 0;
    static constexpr uint32_t WRITER_PENDING = 1u << 1;
    static constexpr uint32_t WRITER_PARKED  = 1u << 2;
    static constexpr uint32_t READER_PARKED  = 1u << 3;
    static constexpr uint32_t READER_SHIFT   = 4u;
    static constexpr uint32_t READER_UNIT    = 1u << READER_SHIFT;

    ZHLN::Atomic<uint32_t> _state {0};

    [[gnu::cold, gnu::noinline]] void LockSlow() noexcept;
    [[gnu::cold, gnu::noinline]] void UnlockSlow() noexcept;
    [[gnu::cold, gnu::noinline]] void LockSharedSlow() noexcept;
    [[gnu::cold, gnu::noinline]] void UnlockSharedSlow() noexcept;
};

static_assert(sizeof(SharedMutex) == sizeof(uint32_t));
static_assert(std::is_standard_layout_v<SharedMutex>);

struct SharedLockGuard {
    SharedMutex& _mutex;

    explicit SharedLockGuard(SharedMutex& mutex) noexcept: _mutex(mutex) {
        _mutex.lock_shared();
    }

    ~SharedLockGuard() noexcept {
        _mutex.unlock_shared();
    }

    SharedLockGuard(const SharedLockGuard&)                    = delete;
    auto operator=(const SharedLockGuard&) -> SharedLockGuard& = delete;
};

struct WriteLockGuard {
    SharedMutex& _mutex;

    explicit WriteLockGuard(SharedMutex& mutex) noexcept: _mutex(mutex) {
        _mutex.lock();
    }

    ~WriteLockGuard() noexcept {
        _mutex.unlock();
    }

    WriteLockGuard(const WriteLockGuard&)                    = delete;
    auto operator=(const WriteLockGuard&) -> WriteLockGuard& = delete;
};

template <typename Func>
decltype(auto) SharedLock(SharedMutex& mutex, Func&& func) {
    SharedLockGuard guard(mutex);
    return std::forward<Func>(func)();
}

template <typename Func>
decltype(auto) WriteLock(SharedMutex& mutex, Func&& func) {
    WriteLockGuard guard(mutex);
    return std::forward<Func>(func)();
}

} // namespace ZHLN
