// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Threading/ConditionalVariable.hpp>
#include <Zahlen/Threading/Mutex.hpp>
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
    Success = 0,
    ConcurrencyFailure[[= ZHLN::Reflect::Description("A race condition was detected; mutual exclusion violated.")]],
    SignalingFailure[[= ZHLN::Reflect::Description("Condition variable signaling failed or timed out.")]],
};

// ============================================================================
// Test Suite Class
// ============================================================================

struct SyncPrimTestSuite {
    SyncPrimTestSuite() {
        // Convert main thread to a Fiber context so that Mutex slow-path yielding works correctly
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, 131072);
    }

    ~SyncPrimTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        // 1. Basic Mutex Locking/Unlocking Properties
        std::expected<void, ZHLN::Error> mutex_basic_lock_unlock() {
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
        std::expected<void, ZHLN::Error> mutex_mutual_exclusion_stress() {
            ZHLN::Mutex mutex;
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
        std::expected<void, ZHLN::Error> condvar_notify_one() {
            ZHLN::Mutex               mutex;
            ZHLN::ConditionalVariable cv;
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
        std::expected<void, ZHLN::Error> condvar_notify_all() {
            ZHLN::Mutex               mutex;
            ZHLN::ConditionalVariable cv;
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
    };
};

int main() {
    return ZHLN::Test::Runner::Run<SyncPrimTestSuite>();
}
