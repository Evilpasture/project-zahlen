// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <array>
#include <atomic>
#include <expected>
#include <vector>

// ============================================================================
// Local Test Enums (Self-Contained)
// ============================================================================

enum class TaskSystemError : uint32_t {
    Success = 0,
    DispatchFailed[[= ZHLN::Reflect::Description("Dispatched tasks failed to execute or update shared memory.")]],
    ParallelForFailed[[= ZHLN::Reflect::Description("ParallelFor processing failed to reach or verify all iterations.")]]
};

// ============================================================================
// Test Suite Class
// ============================================================================

struct TaskSystemTestSuite {
    TaskSystemTestSuite() {
        // Setup: Initialize the fiber scheduling environment (2 threads, 32 fibers, 128KB stack)
        ZHLN::TaskSystem::Init(2, 32, 131072);
    }

    ~TaskSystemTestSuite() {
        // Teardown: Reclaim all scheduler resources
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        std::expected<void, ZHLN::Error> dispatch_and_wait() {
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

        std::expected<void, ZHLN::Error> parallel_for_processing() {
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

        std::expected<void, ZHLN::Error> nested_parallel_for_survives_fiber_pool_saturation() {
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

int main() {
    return ZHLN::Test::Runner::Run<TaskSystemTestSuite>();
}
