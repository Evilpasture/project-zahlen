// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/FunctionRef.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <array>
#include <atomic>
#include <expected>
#include <type_traits>
#include <vector>

// ============================================================================
// Local Test Enums (Self-Contained)
// ============================================================================

enum class TaskSystemError : uint32_t {
    DispatchFailed ZHLN_ANNOTATION(ZHLN::Description<"Dispatched tasks failed to execute or update shared memory.">{}) = 1,
    ParallelForFailed ZHLN_ANNOTATION(ZHLN::Description<"ParallelFor processing failed to reach or verify all iterations.">{}),
    BorrowedTaskFailed ZHLN_ANNOTATION(ZHLN::Description<"Borrowed fiber tasks did not complete before their callables expired.">{})
};

// ============================================================================
// Test Suite Class
// ============================================================================

struct TaskSystemTestSuite {
    TaskSystemTestSuite() {
        // Setup: Initialize the fiber scheduling environment with the guarded minimum stack.
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~TaskSystemTestSuite() {
        // Teardown: Reclaim all scheduler resources
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
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

        std::expected<void, ZHLN::ErrorCode> borrowed_dispatch_waits_for_stack_callables() {
            using Borrow = ZHLN::FunctionRef<void(uint32_t) const>;
            static_assert(std::is_trivially_copyable_v<Borrow>);

            std::atomic<uint32_t> total {0};
            const auto add = [&](uint32_t value) { total.fetch_add(value, std::memory_order_relaxed); };
            static_assert(!std::is_constructible_v<Borrow, decltype(add)&&>); // no borrowing a temporary
            const Borrow view {add};
            view(1);

            // The outer worker borrows two closures in a nested dispatch. Both
            // waits must finish before the corresponding stack views expire.
            const auto nested = [&] {
                ZHLN::TaskSystem::RunBorrowed(
                    [&] { total.fetch_add(2, std::memory_order_relaxed); },
                    [&] { total.fetch_add(4, std::memory_order_relaxed); }
                );
            };
            ZHLN::TaskSystem::RunBorrowed(nested, [&] { total.fetch_add(8, std::memory_order_relaxed); });
            if (!ZHLN::Test::ExpectEq(total.load(std::memory_order_relaxed), 15u)) {
                return std::unexpected(TaskSystemError::BorrowedTaskFailed);
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

