// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Threading/ConditionalVariable.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <bit>
#include <cassert>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <new>
#include <print>

namespace ZHLN {

namespace {

template <bool Debug>
struct LockGraph {
    static constexpr size_t MAX_EDGES = 4096;
    static constexpr size_t MAX_HELD  = 64;

    struct Edge {
        const Mutex* from;
        const Mutex* to;
    };

    struct Empty {};

    struct Graph {
        std::mutex mutex;
        Edge       edges[MAX_EDGES] {};
        size_t     numEdges = 0;
    };

    struct Held {
        const Mutex* locks[MAX_HELD] {};
        size_t       count = 0;
    };

    using State     = std::conditional_t<Debug, Graph, Empty>;
    using HeldStack = std::conditional_t<Debug, Held, Empty>;

    static State s_graph;

    static thread_local HeldStack s_held;

    [[nodiscard]] static auto ContextId() noexcept -> uintptr_t {
        if constexpr (!Debug) {
            return 0;
        } else {
            Fiber* f = GetCurrentFiber();
            if (f != nullptr) {
                return std::bit_cast<uintptr_t>(f);
            }
            thread_local char osThreadTag;
            return std::bit_cast<uintptr_t>(&osThreadTag);
        }
    }

    [[nodiscard]] static auto Reaches(const Mutex* current, const Mutex* target) noexcept -> bool {
        if constexpr (!Debug) {
            return false;
        } else {
            if (current == target) {
                return true;
            }
            for (size_t i = 0; i < s_graph.numEdges; i++) {
                if (s_graph.edges[i].from == current && Reaches(s_graph.edges[i].to, target)) {
                    return true;
                }
            }
            return false;
        }
    }

    static void AddEdge(const Mutex* from, const Mutex* to) noexcept {
        if constexpr (Debug) {
            std::lock_guard<std::mutex> guard(s_graph.mutex);

            for (size_t i = 0; i < s_graph.numEdges; i++) {
                if (s_graph.edges[i].from == from && s_graph.edges[i].to == to) {
                    return;
                }
            }

            if (Reaches(to, from)) {
                std::println(stderr, "[ZHLN FATAL] DEADLOCK DETECTED: Lock order cycle/inversion!");
                std::abort();
            }

            if (s_graph.numEdges < MAX_EDGES) {
                s_graph.edges[s_graph.numEdges++] = {.from = from, .to = to};
            }
        }
    }

    static void RecordAcquire(const Mutex* mutex) noexcept {
        if constexpr (Debug) {
            for (size_t i = 0; i < s_held.count; i++) {
                AddEdge(s_held.locks[i], mutex);
            }
            if (s_held.count < MAX_HELD) {
                s_held.locks[s_held.count++] = mutex;
            }
        }
    }

    static void RecordRelease(const Mutex* mutex) noexcept {
        if constexpr (Debug) {
            for (size_t i = s_held.count; i > 0; i--) {
                if (s_held.locks[i - 1] == mutex) {
                    for (size_t j = i - 1; j < s_held.count - 1; j++) {
                        s_held.locks[j] = s_held.locks[j + 1];
                    }
                    s_held.count--;
                    break;
                }
            }
        }
    }
};

template <bool Debug>
typename LockGraph<Debug>::State LockGraph<Debug>::s_graph {};

template <bool Debug>
thread_local typename LockGraph<Debug>::HeldStack LockGraph<Debug>::s_held {};

using Detector = LockGraph<isDebug>;

}


void Mutex::ClearOwner() noexcept {
    if constexpr (isDebug) {
        _owner.Reset();
    }
}

void Mutex::CheckPreLock() noexcept {
    if constexpr (isDebug) {
        uint8_t bits = _bits.load(std::memory_order::relaxed);
        if (bits & POISONED) [[unlikely]] {
            std::println(stderr, "[ZHLN FATAL] Attempting to lock a POISONED mutex!");
            std::abort();
        }

        if ((bits & LOCKED) && _owner.OwnedBy(Detector::ContextId())) {
            std::println(stderr, "[ZHLN FATAL] DEADLOCK DETECTED: Recursive locking!");
            std::abort();
        }
    }
}

void Mutex::PostLock() noexcept {
    if constexpr (isDebug) {
        _owner.SetOwner(Detector::ContextId());
        Detector::RecordAcquire(this);
    }
}

void Mutex::PreUnlock() noexcept {
    if constexpr (isDebug) {
        if (!_owner.IsOwned()) [[unlikely]] {
            std::println(stderr, "[ZHLN FATAL] Unlocking a mutex that has no owner!");
            std::abort();
        }
        if (!_owner.OwnedBy(Detector::ContextId())) [[unlikely]] {
            std::println(stderr, "[ZHLN FATAL] Unlocking a mutex owned by another thread!");
            std::abort();
        }

        Detector::RecordRelease(this);
    }
}

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

    auto hash = Mix64(std::bit_cast<uint64_t>(addr));

    constexpr int BITS = std::countr_zero(BUCKET_COUNT);

    return static_cast<size_t>(hash >> (64 - BITS));
}


void Mutex::LockSlow() noexcept {
    size_t  hash          = HashAddress<BUCKET_COUNT>(this);
    Bucket* bucket        = &s_parkingLot[hash];
    size_t  backoff_limit = 1;

    for (int i = 0; i < MAX_SPIN_COUNT; i++) {
        uint8_t val = _bits.load(std::memory_order::relaxed);

        if (!(val & LOCKED)) {
            if (_bits.compare_exchange_weak(val, val | LOCKED, std::memory_order::acquire, std::memory_order::relaxed)) {
                if constexpr (isDebug) {
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
                if constexpr (isDebug) {
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

        Fiber* self = GetCurrentFiber();

        bool is_worker_fiber = (self != nullptr && !self->isMain);

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

    size_t                      hash   = HashAddress<BUCKET_COUNT>(this);
    Bucket*                     bucket = &s_parkingLot[hash];
    std::lock_guard<std::mutex> lock(bucket->mutex);

    Waiter** curr    = &bucket->head;
    Waiter*  to_wake = nullptr;
    bool     more    = false;

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

    if (to_wake != nullptr) {
        to_wake->signaled.store(true, std::memory_order::release);
        if (to_wake->fiber == nullptr) {
            to_wake->cond.notify_one();
        } else {
            ZHLN::TaskSystem::WakeUp(to_wake->fiber);
        }
    }
}

void ConditionalVariable::Wait(Mutex& mutex) noexcept {
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

void ConditionalVariable::NotifyOne() noexcept {
    if (_bits.load(std::memory_order::relaxed) == 0) {
        return;
    }

    size_t  hash   = HashAddress<BUCKET_COUNT>(this);
    Bucket* bucket = &s_parkingLot[hash];

    std::lock_guard<std::mutex> lock(bucket->mutex);

    Waiter** curr    = &bucket->head;
    Waiter*  to_wake = nullptr;
    bool     more    = false;

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

    if (to_wake != nullptr) {
        to_wake->signaled.store(true, std::memory_order::release);
        if (to_wake->fiber == nullptr) {
            to_wake->cond.notify_one();
        } else {
            ZHLN::TaskSystem::WakeUp(to_wake->fiber);
        }
    }
}

void ConditionalVariable::NotifyAll() noexcept {
    if (_bits.load(std::memory_order::relaxed) == 0) {
        return;
    }

    size_t  hash   = HashAddress<BUCKET_COUNT>(this);
    Bucket* bucket = &s_parkingLot[hash];

    std::lock_guard<std::mutex> lock(bucket->mutex);

    Waiter** curr      = &bucket->head;
    Waiter*  wake_list = nullptr;

    while (*curr != nullptr) {
        if ((*curr)->address == this) {
            Waiter* waiter = *curr;
            *curr          = waiter->next;

            waiter->next = wake_list;
            wake_list    = waiter;
        } else {
            curr = &((*curr)->next);
        }
    }

    _bits.store(0, std::memory_order::relaxed);

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
