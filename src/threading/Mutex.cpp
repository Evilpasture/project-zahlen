// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

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

// ============================================================================
// Deadlock Detector
// ============================================================================
//
// A class template rather than an #ifdef: LockGraph<isDebug> is the real
// wait-for graph in a debug build and a stateless no-op in a release one. The
// graph is a static data member of a class template, so it is instantiated --
// and takes up static storage -- only when something uses it, which in a
// release build is nothing at all. Same trick for the per-thread stack of
// locks currently held.
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

    // Empty (and unallocated) unless this is a debug build.
    using State     = std::conditional_t<Debug, Graph, Empty>;
    using HeldStack = std::conditional_t<Debug, Held, Empty>;

    // The wait-for graph, shared by every scheduling context: this is what
    // catches an inversion between two contexts that never run at once.
    static State s_graph;

    // The locks this context currently holds, newest last.
    static thread_local HeldStack s_held;

    /**
     * @brief Identifies the scheduling context the caller is running on.
     * Falls back to a dummy thread_local's address to give plain OS threads a
     * guaranteed unique ID, and prefers the Fiber pointer inside one.
     */
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

    /**
     * @brief Records that the caller now holds `mutex`: an edge from every lock
     * it already holds to this one, then push it onto the held stack.
     */
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

    /**
     * @brief Records that the caller has released `mutex`, popping it off the
     * held stack.
     */
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

} // namespace

// ============================================================================
// Debug Hooks
// ============================================================================
// `if constexpr` instead of #ifdef: every check below is compiled in both
// configurations, so it cannot rot the way a preprocessor-excluded copy can,
// and a release build still pays nothing -- the call sites are themselves
// `if constexpr (isDebug)` branches, and the bodies here compile to nothing.

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

/**
 * Fibonacci Hash for pointer addresses.
 * constexpr ensures zero runtime overhead for constant addresses.
 */
template <size_t BUCKET_COUNT>
[[nodiscard]] constexpr size_t HashAddress(const void* addr) noexcept {
    static_assert(std::has_single_bit(BUCKET_COUNT), "BUCKET_COUNT must be a power of two.");

    // Golden ratio for 64-bit distribution
    constexpr uint64_t K    = 0x9E3779B97F4A7C15ULL;
    auto               hash = std::bit_cast<uintptr_t>(addr);

    hash *= K;

    // Use C++20 countr_zero for a safe, constexpr shift calculation
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

    // PHASE 1: Adaptive Exponential Backoff
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

    // PHASE 2: Parking
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

        // A fiber can only yield if it has a caller (meaning it's a worker)
        // If self->isMain is true, we are on the OS root stack.
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
            // Main Thread / OS Thread: Block using Condition Variable
            node.cond.wait(lock, [&]() { return node.signaled.load(std::memory_order::acquire); });
        } else {
            // worker Fiber: Yield back to the scheduler
            lock.unlock();
            while (!node.signaled.load(std::memory_order::acquire)) {
                YieldFiber();
            }

            // FIX: Reset the signal state so that if the lock was stolen
            // by another thread while we were waking up, we can safely
            // park and yield again on the next loop iteration!
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
            // Push the fiber back into the OS Thread Ready Queue!
            ZHLN::TaskSystem::WakeUp(to_wake->fiber);
        }
    }
}

void ConditionalVariable::Wait(Mutex& mutex) noexcept {
    size_t  hash   = HashAddress<BUCKET_COUNT>(this);
    Bucket* bucket = &s_parkingLot[hash];

    Fiber* self            = GetCurrentFiber();
    bool   is_worker_fiber = (self != nullptr && !self->isMain);

    // Prepare the waiter node on the active stack.
    // The memory remains valid while the thread/fiber is blocked.
    Waiter node;
    node.address = this;
    node.fiber   = is_worker_fiber ? self : nullptr;
    node.next    = nullptr;
    node.signaled.store(false, std::memory_order::relaxed);

    // Fast-path hint: let signalers know someone is waiting
    _bits.store(1, std::memory_order::relaxed);

    std::unique_lock<std::mutex> bucket_lock(bucket->mutex);
    node.next    = bucket->head;
    bucket->head = &node;

    if (!is_worker_fiber) {
        // === OS THREAD PATH ===
        bucket_lock.unlock();
        mutex.unlock(); // Release user's mutex to avoid deadlocks

        // Re-lock the bucket to wait on the condition variable safely
        bucket_lock.lock();
        node.cond.wait(bucket_lock, [&]() { return node.signaled.load(std::memory_order::acquire); });
        bucket_lock.unlock();
    } else {
        // === FIBER PATH ===
        bucket_lock.unlock(); // Drop the bucket lock immediately
        mutex.unlock();       // Release user's mutex

        // Yield execution to the fiber scheduler until signaled
        while (!node.signaled.load(std::memory_order::acquire)) {
            YieldFiber();
        }

        // FIX: Reset the state to prevent infinite non-yielding spins
        node.signaled.store(false, std::memory_order::relaxed);
    }

    // Re-acquire the user's mutex before returning to the caller
    mutex.lock();
}

void ConditionalVariable::NotifyOne() noexcept {
    // Fast path: if no waiters exist, bail out immediately
    if (_bits.load(std::memory_order::relaxed) == 0) {
        return;
    }

    size_t  hash   = HashAddress<BUCKET_COUNT>(this);
    Bucket* bucket = &s_parkingLot[hash];

    std::lock_guard<std::mutex> lock(bucket->mutex);

    Waiter** curr    = &bucket->head;
    Waiter*  to_wake = nullptr;
    bool     more    = false;

    // Search and extract the first node waiting on this specific CV address
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
            // Signal the OS thread condition variable
            to_wake->cond.notify_one();
        } else {
            // Wake up the scheduler fiber
            ZHLN::TaskSystem::WakeUp(to_wake->fiber);
        }
    }
}

void ConditionalVariable::NotifyAll() noexcept {
    // Fast path: bail out if no active waiters
    if (_bits.load(std::memory_order::relaxed) == 0) {
        return;
    }

    size_t  hash   = HashAddress<BUCKET_COUNT>(this);
    Bucket* bucket = &s_parkingLot[hash];

    std::lock_guard<std::mutex> lock(bucket->mutex);

    Waiter** curr      = &bucket->head;
    Waiter*  wake_list = nullptr;

    // Isolate and extract all nodes matching this condition variable address
    while (*curr != nullptr) {
        if ((*curr)->address == this) {
            Waiter* waiter = *curr;
            *curr          = waiter->next; // Unlink from bucket

            waiter->next = wake_list; // Link to local stack list
            wake_list    = waiter;
        } else {
            curr = &((*curr)->next);
        }
    }

    _bits.store(0, std::memory_order::relaxed);

    // Unblock all extracted nodes outside the primary bucket list structure
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

} // namespace ZHLN
