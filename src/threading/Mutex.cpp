// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Mutex.hpp"
#include "ConditionalVariable.hpp"
#include "TaskSystem.hpp"
#include "Thread.hpp"
#include <bit>
#include <cassert>
#include <condition_variable>
#include <mutex>
#ifdef ZHLN_DEBUG
#include <print>
#endif
#include <new>
#include <thread>

namespace ZHLN {

// ============================================================================
// Deadlock Detector (Debug Only)
// ============================================================================
#ifdef ZHLN_DEBUG
constexpr size_t MAX_DEBUG_EDGES = 4096;
constexpr size_t MAX_HELD_LOCKS  = 64;

static std::mutex s_debugGraphMutex;
struct Edge {
    const Mutex* from;
    const Mutex* to;
};
static Edge   s_lockEdges[MAX_DEBUG_EDGES];
static size_t s_numLockEdges = 0;

thread_local const Mutex* t_heldLocks[MAX_HELD_LOCKS];
thread_local size_t       t_heldLocksCount = 0;

// Uses a dummy thread_local to generate a guaranteed unique ID for OS Threads,
// and falls back to the Fiber pointer if running inside a Fiber.
static uintptr_t GetCurrentContextId() noexcept {
    Fiber* f = GetCurrentFiber();
    if (f != nullptr) {
        return std::bit_cast<uintptr_t>(f);
    }
    thread_local char osThreadTag;
    return std::bit_cast<uintptr_t>(&osThreadTag);
}

static bool DfsCheckCycle(const Mutex* current, const Mutex* target) noexcept {
    if (current == target) {
        return true;
    }
    for (size_t i = 0; i < s_numLockEdges; i++) {
        if (s_lockEdges[i].from == current) {
            if (DfsCheckCycle(s_lockEdges[i].to, target)) {
                return true;
            }
        }
    }
    return false;
}

static void AddLockEdge(const Mutex* from, const Mutex* to) noexcept {
    std::lock_guard<std::mutex> guard(s_debugGraphMutex);

    for (size_t i = 0; i < s_numLockEdges; i++) {
        if (s_lockEdges[i].from == from && s_lockEdges[i].to == to) {
            return;
        }
    }

    if (DfsCheckCycle(to, from)) {
        std::println(stderr, "[ZHLN FATAL] DEADLOCK DETECTED: Lock order cycle/inversion!");
        std::abort();
    }

    if (s_numLockEdges < MAX_DEBUG_EDGES) {
        s_lockEdges[s_numLockEdges++] = {.from = from, .to = to};
    }
}

void Mutex::ClearOwner() noexcept {
    _hasOwner.store(false, std::memory_order::release);
}

void Mutex::CheckPreLock() noexcept {
    uint8_t bits = _bits.load(std::memory_order::relaxed);
    if (bits & POISONED) [[unlikely]] {
        std::println(stderr, "[ZHLN FATAL] Attempting to lock a POISONED mutex!");
        std::abort();
    }

    if (bits & LOCKED) {
        if (_hasOwner.load(std::memory_order::acquire)) {
            if (GetCurrentContextId() == _owner.load(std::memory_order::relaxed)) {
                std::println(stderr, "[ZHLN FATAL] DEADLOCK DETECTED: Recursive locking!");
                std::abort();
            }
        }
    }
}

void Mutex::PostLock() noexcept {
    _owner.store(GetCurrentContextId(), std::memory_order::relaxed);
    _hasOwner.store(true, std::memory_order::release);

    for (size_t i = 0; i < t_heldLocksCount; i++) {
        AddLockEdge(t_heldLocks[i], this);
    }
    if (t_heldLocksCount < MAX_HELD_LOCKS) {
        t_heldLocks[t_heldLocksCount++] = this;
    }
}

void Mutex::PreUnlock() noexcept {
    if (!_hasOwner.load(std::memory_order::acquire)) [[unlikely]] {
        std::println(stderr, "[ZHLN FATAL] Unlocking a mutex that has no owner!");
        std::abort();
    }
    if (GetCurrentContextId() != _owner.load(std::memory_order::relaxed)) [[unlikely]] {
        std::println(stderr, "[ZHLN FATAL] Unlocking a mutex owned by another thread!");
        std::abort();
    }

    for (size_t i = t_heldLocksCount; i > 0; i--) {
        if (t_heldLocks[i - 1] == this) {
            for (size_t j = i - 1; j < t_heldLocksCount - 1; j++) {
                t_heldLocks[j] = t_heldLocks[j + 1];
            }
            t_heldLocksCount--;
            break;
        }
    }
}
#endif

// ============================================================================
// Parking Lot Configuration
// ============================================================================
constexpr int    MAX_SPIN_COUNT = 40;
constexpr size_t BUCKET_COUNT   = 256;

#if defined(__cpp_lib_hardware_interference_size)
constexpr size_t CACHE_LINE = std::hardware_destructive_interference_size;
#else
constexpr size_t CACHE_LINE = 64;
#endif

struct alignas(CACHE_LINE) Waiter {
    const void*             address;
    Fiber*                  fiber;
    Waiter*                 next;
    std::condition_variable cond;
    ZHLN::Atomic<bool>      signaled;
};

struct alignas(128) Bucket {
    std::mutex mutex;
    Waiter*    head = nullptr;
};

alignas(128) static Bucket s_parkingLot[BUCKET_COUNT];

template <size_t BUCKET_COUNT>
[[nodiscard]] constexpr size_t HashAddress(const void* addr) noexcept {
    static_assert(std::has_single_bit(BUCKET_COUNT), "BUCKET_COUNT must be a power of two.");

    constexpr uint64_t K    = 0x9E3779B97F4A7C15ULL;
    auto               hash = std::bit_cast<uintptr_t>(addr);

    hash *= K;

    constexpr int BITS = std::countr_zero(BUCKET_COUNT);

    return static_cast<size_t>(hash >> (64 - BITS));
}

// ============================================================================
// Slow Path Implementations
// ============================================================================

void Mutex::LockSlow() noexcept {
    size_t  hash          = HashAddress<BUCKET_COUNT>(this);
    Bucket* bucket        = &s_parkingLot[hash];
    size_t  backoff_limit = 1;

    for (int i = 0; i < MAX_SPIN_COUNT; i++) {
        uint8_t val = _bits.load(std::memory_order::relaxed);

        if (!(val & LOCKED)) {
            if (_bits.compare_exchange_weak(val, val | LOCKED, std::memory_order::acquire, std::memory_order::relaxed)) {
                if constexpr (kIsDebugMutex) {
                    PostLock();
                }
                return;
            }
        }

        if (val & POISONED) [[unlikely]] {
            return;
        }

        for (size_t j = 0; j < backoff_limit; j++) {
            CPURelax();
        }
        if (backoff_limit < 1024) {
            backoff_limit <<= 1;
        }
    }

    for (;;) {
        uint8_t val = _bits.load(std::memory_order::relaxed);

        if (!(val & LOCKED)) {
            if (_bits.compare_exchange_weak(val, val | LOCKED, std::memory_order::acquire, std::memory_order::relaxed)) {
                if constexpr (kIsDebugMutex) {
                    PostLock();
                }
                return;
            }
            continue;
        }

        if (!(val & HAS_WAITERS)) {
            if (!_bits.compare_exchange_weak(val, val | HAS_WAITERS, std::memory_order::relaxed, std::memory_order::relaxed)) {
                continue;
            }
        }

        Fiber* self            = GetCurrentFiber();
        bool   is_worker_fiber = (self != nullptr && !self->isMain);

        Waiter node;
        node.address = this;
        node.fiber   = is_worker_fiber ? self : nullptr;
        node.next    = nullptr;
        node.signaled.store(false, std::memory_order::relaxed);

        std::unique_lock<std::mutex> lock(bucket->mutex);

        val = _bits.load(std::memory_order::relaxed);
        if (!(val & LOCKED) || !(val & HAS_WAITERS)) [[unlikely]] {
            lock.unlock();
            continue;
        }

        node.next    = bucket->head;
        bucket->head = &node;

        if (!is_worker_fiber) {
            node.cond.wait(lock, [&]() { return node.signaled.load(std::memory_order::acquire); });
        } else {
            lock.unlock();
            while (!node.signaled.load(std::memory_order::acquire)) {
                YieldFiber();
            }
            node.signaled.store(false, std::memory_order::relaxed);
        }
    }
}

void Mutex::UnlockSlow() noexcept {
    uint8_t val = _bits.load(std::memory_order::relaxed);
    for (;;) {
        uint8_t desired = val & ~LOCKED;
        if (_bits.compare_exchange_weak(val, desired, std::memory_order::release, std::memory_order::relaxed)) {
            if (!(val & HAS_WAITERS)) {
                return;
            }
            break;
        }
    }

    size_t  hash   = HashAddress<BUCKET_COUNT>(this);
    Bucket* bucket = &s_parkingLot[hash];

    Waiter* to_wake = nullptr;
    bool    more    = false;

    {
        std::lock_guard<std::mutex> lock(bucket->mutex);

        Waiter** curr = &bucket->head;
        while (*curr != nullptr) {
            if ((*curr)->address == this && to_wake == nullptr) {
                to_wake = *curr;
                *curr   = to_wake->next;
                continue;
            }
            if ((*curr)->address == this) {
                more = true;
            }
            curr = &((*curr)->next);
        }

        if (!more) {
            val = _bits.load(std::memory_order::relaxed);
            for (;;) {
                if (_bits.compare_exchange_weak(val, val & ~HAS_WAITERS, std::memory_order::relaxed, std::memory_order::relaxed)) {
                    break;
                }
            }
        }

        // --- SIGNAL INSIDE THE LOCK ---
        if (to_wake != nullptr) {
            to_wake->signaled.store(true, std::memory_order::release);
            if (to_wake->fiber == nullptr) {
                to_wake->cond.notify_one();
            } else {
                ZHLN::TaskSystem::WakeUp(to_wake->fiber);
            }
        }
    }
}

void ConditionalVariable::wait(Mutex& mutex) noexcept {
    size_t  hash   = HashAddress<BUCKET_COUNT>(this);
    Bucket* bucket = &s_parkingLot[hash];

    Fiber* self            = GetCurrentFiber();
    bool   is_worker_fiber = (self != nullptr && !self->isMain);

    Waiter node;
    node.address = this;
    node.fiber   = is_worker_fiber ? self : nullptr;
    node.next    = nullptr;
    node.signaled.store(false, std::memory_order::relaxed);

    _bits.store(1, std::memory_order::relaxed);

    std::unique_lock<std::mutex> bucket_lock(bucket->mutex);
    node.next    = bucket->head;
    bucket->head = &node;

    if (!is_worker_fiber) {
        bucket_lock.unlock();
        mutex.unlock();

        bucket_lock.lock();
        node.cond.wait(bucket_lock, [&]() { return node.signaled.load(std::memory_order::acquire); });
        bucket_lock.unlock();
    } else {
        bucket_lock.unlock();
        mutex.unlock();

        while (!node.signaled.load(std::memory_order::acquire)) {
            YieldFiber();
        }

        node.signaled.store(false, std::memory_order::relaxed);
    }

    mutex.lock();
}

void ConditionalVariable::wait(std::unique_lock<Mutex>& lock) noexcept {
    size_t  hash   = HashAddress<BUCKET_COUNT>(this);
    Bucket* bucket = &s_parkingLot[hash];

    Fiber* self            = GetCurrentFiber();
    bool   is_worker_fiber = (self != nullptr && !self->isMain);

    Waiter node;
    node.address = this;
    node.fiber   = is_worker_fiber ? self : nullptr;
    node.next    = nullptr;
    node.signaled.store(false, std::memory_order::relaxed);

    _bits.store(1, std::memory_order::relaxed);

    std::unique_lock<std::mutex> bucket_lock(bucket->mutex);
    node.next    = bucket->head;
    bucket->head = &node;

    if (!is_worker_fiber) {
        bucket_lock.unlock();
        lock.unlock();

        bucket_lock.lock();
        node.cond.wait(bucket_lock, [&]() { return node.signaled.load(std::memory_order::acquire); });
        bucket_lock.unlock();
    } else {
        bucket_lock.unlock();
        lock.unlock();

        while (!node.signaled.load(std::memory_order::acquire)) {
            YieldFiber();
        }

        node.signaled.store(false, std::memory_order::relaxed);
    }

    lock.lock();
}

void ConditionalVariable::notify_one() noexcept {
    if (_bits.load(std::memory_order::relaxed) == 0) {
        return;
    }

    size_t  hash   = HashAddress<BUCKET_COUNT>(this);
    Bucket* bucket = &s_parkingLot[hash];

    Waiter* to_wake = nullptr;
    bool    more    = false;

    {
        std::lock_guard<std::mutex> lock(bucket->mutex);

        Waiter** curr = &bucket->head;
        while (*curr != nullptr) {
            if ((*curr)->address == this && to_wake == nullptr) {
                to_wake = *curr;
                *curr   = to_wake->next;
                continue;
            }
            if ((*curr)->address == this) {
                more = true;
            }
            curr = &((*curr)->next);
        }

        if (!more) {
            _bits.store(0, std::memory_order::relaxed);
        }

        // --- SIGNAL INSIDE THE LOCK ---
        if (to_wake != nullptr) {
            to_wake->signaled.store(true, std::memory_order::release);
            if (to_wake->fiber == nullptr) {
                to_wake->cond.notify_one();
            } else {
                ZHLN::TaskSystem::WakeUp(to_wake->fiber);
            }
        }
    }
}

void ConditionalVariable::notify_all() noexcept {
    if (_bits.load(std::memory_order::relaxed) == 0) {
        return;
    }

    size_t  hash   = HashAddress<BUCKET_COUNT>(this);
    Bucket* bucket = &s_parkingLot[hash];

    Waiter* wake_list = nullptr;

    {
        std::lock_guard<std::mutex> lock(bucket->mutex);

        Waiter** curr = &bucket->head;
        while (*curr != nullptr) {
            if ((*curr)->address == this) {
                Waiter* waiter = *curr;
                *curr          = waiter->next; // Unlink

                waiter->next = wake_list; // Link to local stack list
                wake_list    = waiter;
            } else {
                curr = &((*curr)->next);
            }
        }

        _bits.store(0, std::memory_order::relaxed);

        // --- SIGNAL INSIDE THE LOCK ---
        while (wake_list != nullptr) {
            Waiter* waiter = wake_list;
            wake_list      = waiter->next;

            waiter->signaled.store(true, std::memory_order::release);
            if (waiter->fiber == nullptr) {
                waiter->cond.notify_one();
            } else {
                ZHLN::TaskSystem::WakeUp(waiter->fiber);
            }
        }
    }
}

} // namespace ZHLN
