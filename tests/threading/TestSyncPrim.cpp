// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Threading/ConditionalVariable.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/SharedMutex.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp> // Required for InitMainThread
#include <array>
#include <cstddef>
#include <expected>
#include <memory>

// ============================================================================
// Local Test Enums (Self-Contained)
// ============================================================================

enum class SyncPrimError : uint32_t {
    ConcurrencyFailure ZHLN_ANNOTATION(ZHLN::Description<"A race condition was detected; mutual exclusion violated.">{}) = 1,
    SignalingFailure ZHLN_ANNOTATION(ZHLN::Description<"Condition variable signaling failed or timed out.">{}),
};

// ============================================================================
// Test Suite Class
// ============================================================================

struct SyncPrimTestSuite {
    SyncPrimTestSuite() {
        // Convert main thread to a Fiber context so that Mutex slow-path yielding works correctly
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~SyncPrimTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        // 1. Basic Mutex Locking/Unlocking Properties
        std::expected<void, ZHLN::ErrorCode> mutex_basic_lock_unlock() {
            // Construction must initialize the lock byte even on dirty stack
            // storage. The old trivial default constructor left this byte
            // indeterminate and caused permanent "locked" states on macOS.
            alignas(ZHLN::Mutex) std::array<std::byte, sizeof(ZHLN::Mutex)> dirtyStorage;
            dirtyStorage.fill(std::byte {0xff});
            auto*      initialized = std::construct_at(reinterpret_cast<ZHLN::Mutex*>(dirtyStorage.data()));
            const bool dirtyTry    = initialized->try_lock();
            ZHLN::Test::ExpectTrue(dirtyTry);
            if (dirtyTry) {
                initialized->unlock();
            }
            std::destroy_at(initialized);

            ZHLN::Mutex mutex {};

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
            bool                      workerFinished = false;

            struct WorkerData {
                ZHLN::Mutex*               m;
                ZHLN::ConditionalVariable* cv;
                bool*                      ready;
                bool*                      finished;
            };

            auto worker_fn = [](void* arg) {
                auto* d = static_cast<WorkerData*>(arg);
                d->m->lock();
                while (!(*d->ready)) {
                    d->cv->Wait(*d->m);
                }
                *d->finished = true;
                d->m->unlock();
            };

            WorkerData             wData {&mutex, &cv, &ready, &workerFinished};
            ZHLN::TaskSystem::Task task {worker_fn, &wData};

            ZHLN::TaskSystem::Counter sync;
            ZHLN::TaskSystem::Dispatch({&task, 1}, &sync);

            // Yield briefly to ensure worker thread has entered the CV wait path
            for (int i = 0; i < 50; ++i) {
                ZHLN::CPURelax();
            }

            mutex.lock();
            ready = true;
            mutex.unlock();
            cv.NotifyOne();

            ZHLN::TaskSystem::Wait(&sync);

            ZHLN::Test::ExpectTrue(workerFinished);
            return {};
        }

        // 4. Condition Variable Multi-Signal (NotifyAll)
        std::expected<void, ZHLN::ErrorCode> condvar_notify_all() {
            ZHLN::Mutex               mutex {};
            ZHLN::ConditionalVariable cv {};
            bool                      ready      = false;
            int                       awakeCount = 0;

            struct WorkerData {
                ZHLN::Mutex*               m;
                ZHLN::ConditionalVariable* cv;
                bool*                      ready;
                int*                       count;
            };

            auto worker_fn = [](void* arg) {
                auto* d = static_cast<WorkerData*>(arg);
                d->m->lock();
                while (!(*d->ready)) {
                    d->cv->Wait(*d->m);
                }
                (*d->count)++;
                d->m->unlock();
            };

            WorkerData                            wData {&mutex, &cv, &ready, &awakeCount};
            std::array<ZHLN::TaskSystem::Task, 4> tasks;
            for (auto& task: tasks) {
                task = {worker_fn, &wData};
            }

            ZHLN::TaskSystem::Counter sync;
            ZHLN::TaskSystem::Dispatch(tasks, &sync);

            // Yield briefly to ensure all workers have blocked inside the parking lot
            for (int i = 0; i < 50; ++i) {
                ZHLN::CPURelax();
            }

            mutex.lock();
            ready = true;
            mutex.unlock();
            cv.NotifyAll();

            ZHLN::TaskSystem::Wait(&sync);

            ZHLN::Test::ExpectEq(awakeCount, 4);
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

            ZHLN::SharedMutex     rwLock {};
            std::atomic<uint32_t> activeReaders {0};
            std::atomic<uint32_t> maxReaders {0};
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
                            const uint32_t cur = activeReaders.fetch_add(1, std::memory_order::acq_rel) + 1;
                            uint32_t        old = maxReaders.load(std::memory_order::relaxed);
                            while (cur > old && !maxReaders.compare_exchange_weak(old, cur, std::memory_order::relaxed)) {
                            }
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

            if (violated.load(std::memory_order::relaxed) || !ZHLN::Test::ExpectEq(sharedValue, 256u) ||
                !ZHLN::Test::ExpectGt(maxReaders.load(std::memory_order::relaxed), 1u)) {
                return std::unexpected(SyncPrimError::ConcurrencyFailure);
            }
            return {};
        }
    };
};

// Exported for the threading group binary (RunThreadingTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunSyncPrimSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<SyncPrimTestSuite>();
}

