// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/FunctionRef.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <expected>
#include <latch>
#include <thread>
#include <type_traits>
#include <vector>

// ============================================================================
// Local Test Enums (Self-Contained)
// ============================================================================

enum class TaskSystemError : uint32_t {
    DispatchFailed ZHLN_ANNOTATION(ZHLN::Description<"Dispatched tasks failed to execute or update shared memory.">{}) = 1,
    ParallelForFailed ZHLN_ANNOTATION(ZHLN::Description<"ParallelFor processing failed to reach or verify all iterations.">{}),
    ParallelInvokeFailed ZHLN_ANNOTATION(ZHLN::Description<"ParallelInvoke returned before its tasks completed.">{}),
    LifecycleFailed ZHLN_ANNOTATION(ZHLN::Description<"Task system lifecycle did not serialize or restart cleanly.">{})
};

static_assert(!std::is_copy_constructible_v<ZHLN::TaskSystem::Scope> && !std::is_copy_assignable_v<ZHLN::TaskSystem::Scope>);
static_assert(!std::is_move_constructible_v<ZHLN::TaskSystem::Scope> && !std::is_move_assignable_v<ZHLN::TaskSystem::Scope>);

// ============================================================================
// Test Suite Class
// ============================================================================

struct TaskSystemTestSuite {
    // Constructed before the runner's Tests object and retired when the suite
    // exits, never from a process-exit static destructor.
    ZHLN::TaskSystem::Scope _tasks {2, 32, ZHLN::kMinimumFiberStackSize};

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> lifecycle_is_idempotent_and_can_restart() {
            namespace tasks = ZHLN::TaskSystem;
            constexpr uint32_t workerThreads = 2;
            constexpr uint32_t fibers = 32;

            // Reinitializing an active scheduler leaves its configuration alone.
            tasks::Init(4, 64, ZHLN::kMinimumFiberStackSize);
            const uint32_t initialCount = tasks::GetWorkerCount();
            tasks::Shutdown();
            tasks::Shutdown();
            const uint32_t firstStopCount = tasks::GetWorkerCount();

            // Each wave starts together; only one caller may construct or
            // destroy the worker threads/fiber pool at a time.
            std::latch initStart {4};
            std::array<std::thread, 4> initCallers;
            for (auto& thread: initCallers) {
                thread = std::thread([&] {
                    initStart.arrive_and_wait();
                    tasks::Init(workerThreads, fibers, ZHLN::kMinimumFiberStackSize);
                });
            }
            for (auto& thread: initCallers) {
                thread.join();
            }
            const uint32_t concurrentInitCount = tasks::GetWorkerCount();

            std::atomic<uint32_t> completed {0};
            const auto add = [](void* raw) { static_cast<std::atomic<uint32_t>*>(raw)->fetch_add(1, std::memory_order_relaxed); };
            std::array<tasks::Task, 4> work;
            for (auto& task: work) {
                task = {.func = add, .arg = &completed};
            }
            if (concurrentInitCount == workerThreads + 1) {
                tasks::Counter counter;
                tasks::Dispatch(work, &counter);
                tasks::Wait(&counter);
            }

            std::latch shutdownStart {4};
            std::array<std::thread, 4> shutdownCallers;
            for (auto& thread: shutdownCallers) {
                thread = std::thread([&] {
                    shutdownStart.arrive_and_wait();
                    tasks::Shutdown();
                });
            }
            for (auto& thread: shutdownCallers) {
                thread.join();
            }
            const uint32_t concurrentStopCount = tasks::GetWorkerCount();

            // Init and Shutdown may also overlap. Whichever transition wins
            // last determines the state, but a partial pool is never visible.
            std::latch mixedStart {4};
            std::array<std::thread, 4> mixedCallers;
            for (size_t i = 0; i < mixedCallers.size(); ++i) {
                mixedCallers[i] = std::thread([&, i] {
                    mixedStart.arrive_and_wait();
                    if (i % 2 == 0) {
                        tasks::Init(workerThreads, fibers, ZHLN::kMinimumFiberStackSize);
                    } else {
                        tasks::Shutdown();
                    }
                });
            }
            for (auto& thread: mixedCallers) {
                thread.join();
            }
            const uint32_t mixedCount = tasks::GetWorkerCount();
            tasks::Shutdown();
            const uint32_t mixedStopCount = tasks::GetWorkerCount();

            // Restore the suite's scheduler for the remaining tests, and
            // exercise both work queues after the wake/reset/join sequence.
            tasks::Init(workerThreads, fibers, ZHLN::kMinimumFiberStackSize);
            const uint32_t restartCount = tasks::GetWorkerCount();
            if (restartCount == workerThreads + 1) {
                tasks::Counter counter;
                tasks::Dispatch(work, &counter);
                tasks::Wait(&counter);
            }

            bool ok = true;
            ok = ZHLN::Test::ExpectEq(initialCount, 3u) && ok;
            ok = ZHLN::Test::ExpectEq(firstStopCount, 0u) && ok;
            ok = ZHLN::Test::ExpectEq(concurrentInitCount, 3u) && ok;
            ok = ZHLN::Test::ExpectEq(concurrentStopCount, 0u) && ok;
            ok = ZHLN::Test::ExpectTrue(mixedCount == 0u || mixedCount == 3u) && ok;
            ok = ZHLN::Test::ExpectEq(mixedStopCount, 0u) && ok;
            ok = ZHLN::Test::ExpectEq(restartCount, 3u) && ok;
            ok = ZHLN::Test::ExpectEq(completed.load(std::memory_order::relaxed), 8u) && ok;
            if (!ok) {
                return std::unexpected(TaskSystemError::LifecycleFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> fiber_metadata_alignment() {
            auto         noop    = [](void*) {};
            ZHLN::Fiber* fiber   = ZHLN::Fiber::Create(ZHLN::kMinimumFiberStackSize, noop, nullptr);
            const bool   aligned = fiber != nullptr && reinterpret_cast<uintptr_t>(fiber) % alignof(ZHLN::Fiber) == 0 &&
                                   fiber->mapSize >= ZHLN::kMinimumFiberStackSize;
            ZHLN::Fiber::Destroy(fiber);
            if (!aligned) {
                return std::unexpected(TaskSystemError::DispatchFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> dispatch_and_wait() {
            std::atomic<int>          accum {0};
            ZHLN::TaskSystem::Counter counter;

            auto task_fn = [](void* arg) {
                auto* a = static_cast<std::atomic<int>*>(arg);
                a->fetch_add(1, std::memory_order::relaxed);
            };

            std::array<ZHLN::TaskSystem::Task, 8> tasks = {
                {{task_fn, &accum},
                 {task_fn, &accum},
                 {task_fn, &accum},
                 {task_fn, &accum},
                 {task_fn, &accum},
                 {task_fn, &accum},
                 {task_fn, &accum},
                 {task_fn, &accum}}
            };

            ZHLN::TaskSystem::Dispatch(tasks, &counter);
            ZHLN::TaskSystem::Wait(&counter);

            if (accum.load(std::memory_order::relaxed) != 8) {
                return std::unexpected(TaskSystemError::DispatchFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> parallel_invoke_joins_stack_callables() {
            std::atomic<uint32_t> total {0};
            const auto add = [&](uint32_t value) { total.fetch_add(value, std::memory_order_relaxed); };
            const ZHLN::FunctionRef<void(uint32_t) const> view {add};
            view(1);

            // The outer worker invokes temporary closures in a nested fork.
            // Both joins must finish before the corresponding stack views expire.
            ZHLN::TaskSystem::ParallelInvoke();
            const auto nested = [&] {
                ZHLN::TaskSystem::ParallelInvoke(
                    [&] { total.fetch_add(2, std::memory_order_relaxed); },
                    [&] { total.fetch_add(4, std::memory_order_relaxed); }
                );
            };
            ZHLN::TaskSystem::ParallelInvoke(nested, [&] { total.fetch_add(8, std::memory_order_relaxed); });
            if (!ZHLN::Test::ExpectEq(total.load(std::memory_order_relaxed), 15u)) {
                return std::unexpected(TaskSystemError::ParallelInvokeFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> parallel_for_processing() {
            constexpr size_t arraySize = 512;
            std::vector<int> data(arraySize, 0);

            // Execute concurrent chunked loops
            ZHLN::TaskSystem::ParallelFor(arraySize, 64, [&](uint32_t start, uint32_t end, uint32_t) {
                for (uint32_t i = start; i < end; ++i) {
                    data[i] = static_cast<int>(i) * 2;
                }
            });

            // Verify integrity
            for (size_t i = 0; i < arraySize; ++i) {
                if (data[i] != static_cast<int>(i) * 2) {
                    return std::unexpected(TaskSystemError::ParallelForFailed);
                }
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> nested_parallel_for_survives_fiber_pool_saturation() {
            constexpr size_t      outerTaskCount = 24;
            constexpr size_t      innerTaskCount = 64;
            std::atomic<uint32_t> completed {0};

            struct Payload {
                std::atomic<uint32_t>* completed;
            } payload {&completed};

            auto outerTask = [](void* raw) {
                auto* value = static_cast<Payload*>(raw);
                ZHLN::TaskSystem::ParallelFor(innerTaskCount, 1, [&](uint32_t start, uint32_t end, uint32_t) {
                    value->completed->fetch_add(end - start, std::memory_order::relaxed);
                });
            };

            std::array<ZHLN::TaskSystem::Task, outerTaskCount> tasks {};
            for (auto& task: tasks) {
                task = {.func = outerTask, .arg = &payload};
            }

            ZHLN::TaskSystem::Counter counter;
            ZHLN::TaskSystem::Dispatch(tasks, &counter);
            ZHLN::TaskSystem::Wait(&counter);
            if (completed.load(std::memory_order::relaxed) != outerTaskCount * innerTaskCount) {
                return std::unexpected(TaskSystemError::ParallelForFailed);
            }
            return {};
        }
    };
};

// Exported for the threading group binary (RunThreadingTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunTaskSystemSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<TaskSystemTestSuite>();
}

