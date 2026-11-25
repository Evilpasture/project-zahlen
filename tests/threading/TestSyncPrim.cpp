// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Threading/ConditionalVariable.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/SharedMutex.hpp>
#include <Zahlen/Threading/Sync.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp> // Required for InitMainThread
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <latch>
#include <memory>
#include <new>
#include <thread>

// ============================================================================
// Local Test Enums (Self-Contained)
// ============================================================================

enum class SyncPrimError : uint32_t {
    ConcurrencyFailure ZHLN_ANNOTATION(ZHLN::Description<"A race condition was detected; mutual exclusion violated.">{}) = 1,
    ConcurrentReadersFailed ZHLN_ANNOTATION(ZHLN::Description<"SharedMutex failed to admit concurrent readers or wake a parked writer.">{}),
    SignalingFailure ZHLN_ANNOTATION(ZHLN::Description<"Condition variable signaling failed or timed out.">{}),
};

// ============================================================================
// Test Suite Class
// ============================================================================

namespace {

struct SyncPrimTestSuite {
    SyncPrimTestSuite() {
        // Convert main thread to a Fiber context so that Mutex slow-path yielding works correctly
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 128, ZHLN::kMinimumFiberStackSize);
    }

    ~SyncPrimTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        // 1. Basic Mutex Locking/Unlocking Properties
        std::expected<void, ZHLN::ErrorCode> mutex_basic_lock_unlock() {
            // Default-initialization (not just value-initialization via
            // std::construct_at) must initialize the lock byte even on dirty
            // heap/stack storage. Testing 0x01 and 0x41 specifically covers
            // bit patterns where LOCKED (0x01) is set and POISONED (0x04) is
            // clear, which previously caused permanent deadlocks in Release.
            struct MemberHolder {
                uint32_t                  tag;
                ZHLN::Mutex               mutex;
                ZHLN::ConditionalVariable cv;
                ZHLN::BufferSync          sync;
                explicit MemberHolder(uint32_t t) noexcept: tag(t) {}
            };

            for (const uint8_t pattern: {uint8_t {0x01}, uint8_t {0x41}, uint8_t {0xff}}) {
                alignas(ZHLN::Mutex) std::array<std::byte, sizeof(ZHLN::Mutex)> dirtyStorage;
                dirtyStorage.fill(std::byte {pattern});
                auto*      initialized = ::new (dirtyStorage.data()) ZHLN::Mutex;
                const bool dirtyTry    = initialized->try_lock();
                ZHLN::Test::ExpectTrue(dirtyTry);
                if (dirtyTry) {
                    initialized->unlock();
                }
                std::destroy_at(initialized);

                alignas(MemberHolder) std::array<std::byte, sizeof(MemberHolder)> dirtyHolder;
                dirtyHolder.fill(std::byte {pattern});
                auto*      holder    = ::new (dirtyHolder.data()) MemberHolder(7u);
                const bool holderTry = holder->mutex.try_lock();
                ZHLN::Test::ExpectTrue(holderTry);
                if (holderTry) {
                    holder->mutex.unlock();
                }
                const bool syncTry = holder->sync.shadowLock.try_lock();
                ZHLN::Test::ExpectTrue(syncTry);
                if (syncTry) {
                    holder->sync.shadowLock.unlock();
                }
                ZHLN::Test::ExpectEq(holder->sync.viewExportCount.load(std::memory_order::relaxed), 0);
                holder->cv.NotifyOne();
                std::destroy_at(holder);
            }

            ZHLN::Mutex mutex;

            // Sequential lock/unlock
            mutex.lock();
            mutex.unlock();

            // try_lock on an unlocked mutex
            bool firstTry = mutex.try_lock();
            ZHLN::Test::ExpectTrue(firstTry);

            if (firstTry) {
                // try_lock on an already locked mutex should fail
                bool secondTry = mutex.try_lock();
                ZHLN::Test::ExpectFalse(secondTry);

                mutex.unlock();
            }

            return {};
        }

        // 2. Stress Test Mutex Mutual Exclusion under High Concurrency
        std::expected<void, ZHLN::ErrorCode> mutex_mutual_exclusion_stress() {
            ZHLN::Mutex mutex {};
            int         counter = 0;

            // Increment shared counter concurrently across worker tasks
            ZHLN::TaskSystem::ParallelFor(16000, 128, [&](uint32_t start, uint32_t end, uint32_t) {
                for (uint32_t i = start; i < end; ++i) {
                    ZHLN::MutexGuard guard(mutex);
                    counter++;
                }
            });

            // If mutual exclusion was violated, counter won't equal 16000
            ZHLN::Test::ExpectEq(counter, 16000);
            return {};
        }

        // 3. Condition Variable Single-Signal (NotifyOne)
        std::expected<void, ZHLN::ErrorCode> condvar_notify_one() {
            ZHLN::Mutex               mutex {};
            ZHLN::ConditionalVariable cv {};
            bool                      ready          = false;
            bool                      workerWaiting  = false;
            bool                      workerFinished = false;

            struct WorkerData {
                ZHLN::Mutex*               m;
                ZHLN::ConditionalVariable* cv;
                bool*                      ready;
                bool*                      waiting;
                bool*                      finished;
            };

            auto worker_fn = [](void* arg) {
                auto* d = static_cast<WorkerData*>(arg);
                d->m->lock();
                *d->waiting = true;
                while (!(*d->ready)) {
                    d->cv->Wait(*d->m);
                }
                *d->finished = true;
                d->m->unlock();
            };

            WorkerData             wData {&mutex, &cv, &ready, &workerWaiting, &workerFinished};
            ZHLN::TaskSystem::Task task {worker_fn, &wData};

            ZHLN::TaskSystem::Counter sync;
            ZHLN::TaskSystem::Dispatch({&task, 1}, &sync);

            // Deterministically wait until the worker acquires `mutex` and releases it inside `cv.Wait(mutex)`.
            for (;;) {
                mutex.lock();
                if (workerWaiting) {
                    ready = true;
                    mutex.unlock();
                    break;
                }
                mutex.unlock();
                std::this_thread::yield();
            }
            cv.NotifyOne();

            ZHLN::TaskSystem::Wait(&sync);

            if (!ZHLN::Test::ExpectTrue(workerFinished)) {
                return std::unexpected(SyncPrimError::SignalingFailure);
            }
            return {};
        }

        // 4. Condition Variable Multi-Signal (NotifyAll)
        std::expected<void, ZHLN::ErrorCode> condvar_notify_all() {
            ZHLN::Mutex               mutex {};
            ZHLN::ConditionalVariable cv {};
            bool                      ready        = false;
            int                       waitingCount = 0;
            int                       awakeCount   = 0;

            struct WorkerData {
                ZHLN::Mutex*               m;
                ZHLN::ConditionalVariable* cv;
                bool*                      ready;
                int*                       waiting;
                int*                       count;
            };

            auto worker_fn = [](void* arg) {
                auto* d = static_cast<WorkerData*>(arg);
                d->m->lock();
                (*d->waiting)++;
                while (!(*d->ready)) {
                    d->cv->Wait(*d->m);
                }
                (*d->count)++;
                d->m->unlock();
            };

            WorkerData                            wData {&mutex, &cv, &ready, &waitingCount, &awakeCount};
            std::array<ZHLN::TaskSystem::Task, 4> tasks;
            for (auto& task: tasks) {
                task = {worker_fn, &wData};
            }

            ZHLN::TaskSystem::Counter sync;
            ZHLN::TaskSystem::Dispatch(tasks, &sync);

            // Deterministically wait until all 4 workers have parked inside `cv.Wait(mutex)`.
            for (;;) {
                mutex.lock();
                if (waitingCount == 4) {
                    ready = true;
                    mutex.unlock();
                    break;
                }
                mutex.unlock();
                std::this_thread::yield();
            }
            cv.NotifyAll();

            ZHLN::TaskSystem::Wait(&sync);

            if (!ZHLN::Test::ExpectEq(awakeCount, 4)) {
                return std::unexpected(SyncPrimError::SignalingFailure);
            }
            return {};
        }

        // 5. SharedMutex Concurrent Readers & Exclusive Writers
        std::expected<void, ZHLN::ErrorCode> shared_mutex_readers_and_writers() {
            alignas(ZHLN::SharedMutex) std::array<std::byte, sizeof(ZHLN::SharedMutex)> dirtyStorage;
            dirtyStorage.fill(std::byte {0xff});
            auto*      initialized = std::construct_at(reinterpret_cast<ZHLN::SharedMutex*>(dirtyStorage.data()));
            const bool dirtyTry    = initialized->try_lock();
            if (!ZHLN::Test::ExpectTrue(dirtyTry)) {
                std::destroy_at(initialized);
                return std::unexpected(SyncPrimError::ConcurrencyFailure);
            }
            initialized->unlock();
            std::destroy_at(initialized);

            // Phase A: Deterministic concurrent-reader rendezvous & parked-writer handoff
            // (independent of OS thread scheduling speed or available vCPU count).
            {
                ZHLN::SharedMutex     rwLock {};
                std::atomic<uint32_t> activeReaders {0};
                std::atomic<uint32_t> peakReaders {0};
                std::atomic<bool>     writerObservedReader {false};
                std::atomic<bool>     writerCompleted {false};
                std::latch            bothReadersInside {2};
                std::latch            releaseReaders {1};
                std::latch            writerStarting {1};

                auto readerFn = [&] {
                    ZHLN::Fiber::InitMainThread();
                    ZHLN::SharedLock(rwLock, [&] {
                        const uint32_t cur = activeReaders.fetch_add(1, std::memory_order::acq_rel) + 1;
                        uint32_t       old = peakReaders.load(std::memory_order::relaxed);
                        while (cur > old && !peakReaders.compare_exchange_weak(old, cur, std::memory_order::relaxed)) {
                        }

                        bothReadersInside.arrive_and_wait();
                        releaseReaders.wait();

                        activeReaders.fetch_sub(1, std::memory_order::acq_rel);
                    });
                };

                std::thread readerA(readerFn);
                std::thread readerB(readerFn);

                bothReadersInside.wait();
                const bool writerTryWhileShared = rwLock.try_lock();

                std::thread writerThread([&] {
                    ZHLN::Fiber::InitMainThread();
                    writerStarting.count_down();
                    ZHLN::WriteLock(rwLock, [&] {
                        if (activeReaders.load(std::memory_order::acquire) != 0u) {
                            writerObservedReader.store(true, std::memory_order::relaxed);
                        }
                        writerCompleted.store(true, std::memory_order::release);
                    });
                });

                writerStarting.wait();
                while (rwLock.try_lock_shared()) {
                    rwLock.unlock_shared();
                    std::this_thread::yield();
                }

                const bool writerPremature = writerCompleted.load(std::memory_order::acquire);

                releaseReaders.count_down();
                readerA.join();
                readerB.join();
                writerThread.join();

                if (!ZHLN::Test::ExpectEq(peakReaders.load(std::memory_order::relaxed), 2u) ||
                    !ZHLN::Test::ExpectFalse(writerTryWhileShared) ||
                    !ZHLN::Test::ExpectFalse(writerPremature) ||
                    !ZHLN::Test::ExpectFalse(writerObservedReader.load(std::memory_order::relaxed)) ||
                    !ZHLN::Test::ExpectTrue(writerCompleted.load(std::memory_order::acquire))) {
                    return std::unexpected(SyncPrimError::ConcurrentReadersFailed);
                }
            }

            // Phase B: High-contention fiber/worker stress test for mutual-exclusion invariants.
            ZHLN::SharedMutex     rwLock {};
            std::atomic<uint32_t> activeReaders {0};
            std::atomic<bool>     writerActive {false};
            std::atomic<bool>     violated {false};
            uint32_t              sharedValue = 0;

            ZHLN::TaskSystem::ParallelFor(4096, 32, [&](uint32_t start, uint32_t end, uint32_t) {
                for (uint32_t i = start; i < end; ++i) {
                    if ((i & 15u) == 0u) {
                        ZHLN::WriteLock(rwLock, [&] {
                            if (writerActive.exchange(true, std::memory_order::acq_rel) ||
                                activeReaders.load(std::memory_order::acquire) != 0u) {
                                violated.store(true, std::memory_order::relaxed);
                            }
                            ++sharedValue;
                            writerActive.store(false, std::memory_order::release);
                        });
                    } else {
                        ZHLN::SharedLock(rwLock, [&] {
                            if (writerActive.load(std::memory_order::acquire)) {
                                violated.store(true, std::memory_order::relaxed);
                            }
                            activeReaders.fetch_add(1, std::memory_order::acq_rel);
                            for (int spin = 0; spin < 8; ++spin) {
                                ZHLN::CPURelax();
                            }
                            const uint32_t snapshot = sharedValue;
                            if (snapshot > 4096u) {
                                violated.store(true, std::memory_order::relaxed);
                            }
                            activeReaders.fetch_sub(1, std::memory_order::acq_rel);
                        });
                    }
                }
            });

            if (violated.load(std::memory_order::relaxed) || !ZHLN::Test::ExpectEq(sharedValue, 256u)) {
                return std::unexpected(SyncPrimError::ConcurrencyFailure);
            }
            return {};
        }

        // 6. Fiber + OS Mutex Churn & ConditionalVariable NotifyOne Wakeup Stress
        std::expected<void, ZHLN::ErrorCode> fiber_mutex_churn_and_condvar_wakeup_stress() {
            // Exercises fiber recycling concurrently with OS-thread mutex unlocks
            // waking fibers across the two-phase WaitState / Fiber::Resume protocol.
            for (int round = 0; round < 32; ++round) {
                ZHLN::Mutex           mutex;
                std::atomic<uint32_t> completed {0};
                std::atomic<bool>     stopHammer {false};

                std::thread osHammer([&] {
                    while (!stopHammer.load(std::memory_order::relaxed)) {
                        ZHLN::MutexGuard guard(mutex);
                        for (int spin = 0; spin < 16; ++spin) {
                            ZHLN::CPURelax();
                        }
                    }
                });

                constexpr uint32_t kTasks = 96;
                ZHLN::TaskSystem::ParallelFor(kTasks, 1, [&](uint32_t start, uint32_t end, uint32_t) {
                    for (uint32_t i = start; i < end; ++i) {
                        ZHLN::MutexGuard guard(mutex);
                        for (int spin = 0; spin < 12; ++spin) {
                            ZHLN::CPURelax();
                        }
                        completed.fetch_add(1, std::memory_order::relaxed);
                    }
                });

                stopHammer.store(true, std::memory_order::relaxed);
                osHammer.join();

                if (!ZHLN::Test::ExpectEq(completed.load(std::memory_order::relaxed), kTasks)) {
                    return std::unexpected(SyncPrimError::ConcurrencyFailure);
                }
            }

            // Exercises concurrent ConditionalVariable::Wait + NotifyOne without
            // losing a wake when _bits transitions while new waiters enqueue.
            for (int round = 0; round < 64; ++round) {
                ZHLN::Mutex               mutex;
                ZHLN::ConditionalVariable cv;
                int                       available = 0;
                std::atomic<int>          consumed {0};
                constexpr int             kConsumers = 4;
                constexpr int             kPerThread = 12;

                std::array<std::thread, kConsumers> consumers;
                for (auto& th: consumers) {
                    th = std::thread([&] {
                        for (int i = 0; i < kPerThread; ++i) {
                            ZHLN::MutexGuard guard(mutex);
                            while (available == 0) {
                                cv.Wait(mutex);
                            }
                            --available;
                            consumed.fetch_add(1, std::memory_order::relaxed);
                        }
                    });
                }

                for (int i = 0; i < kConsumers * kPerThread; ++i) {
                    {
                        ZHLN::MutexGuard guard(mutex);
                        ++available;
                    }
                    cv.NotifyOne();
                }

                for (auto& th: consumers) {
                    th.join();
                }

                if (!ZHLN::Test::ExpectEq(consumed.load(std::memory_order::relaxed), kConsumers * kPerThread)) {
                    return std::unexpected(SyncPrimError::SignalingFailure);
                }
            }

            return {};
        }
    };
};

} // namespace
