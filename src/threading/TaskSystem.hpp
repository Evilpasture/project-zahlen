// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <detail/Atomic.hpp>
#include <span>

namespace ZHLN {
struct Fiber;
}

namespace ZHLN::TaskSystem {
uint32_t GetWorkerIndex();
uint32_t GetWorkerCount();
// The function signature for a Job
using TaskFn = void (*)(void*);

struct Task {
    TaskFn func;
    void*  arg;
};

// A sync point to wait for a batch of jobs to finish
struct Counter {
    ZHLN::Atomic<uint32_t> value {0};
};

/**
 * @brief Boots up the OS worker threads and pre-allocates the Fiber pool.
 * @param numThreads 0 = Auto-detect CPU cores
 */
void Init(uint32_t numThreads = 0, uint32_t numFibers = 128, size_t stackSize = 524288);

/**
 * @brief Shuts down the threads and cleans up memory.
 */
void Shutdown();

/**
 * @brief Kicks off a batch of tasks. Non-blocking.
 * @param tasks An array of Tasks to run.
 * @param counter Optional counter to track completion.
 */
void Dispatch(std::span<const Task> tasks, Counter* counter = nullptr);

/**
 * @brief Yields the current thread/fiber until the counter hits zero.
 */
void Wait(Counter* counter);

/**
 * @brief Internal use. Used by Mutex.cpp to wake up a sleeping Fiber.
 */
void WakeUp(ZHLN::Fiber* fiber);

template <typename Func>
void ParallelFor(uint32_t count, uint32_t chunkSize, Func&& func) {
    if (count == 0) {
        return;
    }
    if (count <= chunkSize) {
        std::forward<Func>(func)(0, count, 0); // start, end, chunkIdx
        return;
    }

    constexpr uint32_t MaxChunks         = 128;
    uint32_t           adjustedChunkSize = chunkSize;
    uint32_t           numChunks         = (count + adjustedChunkSize - 1) / adjustedChunkSize;

    // Rescale chunk size to fit our bounds, then dynamically compute
    // the exact number of active chunks required to avoid launching empty tasks.
    if (numChunks > MaxChunks) {
        adjustedChunkSize = (count + MaxChunks - 1) / MaxChunks;
        numChunks         = (count + adjustedChunkSize - 1) / adjustedChunkSize;
    }

    using DecayedFunc = std::remove_cvref_t<Func>;
    struct ChunkJob {
        const DecayedFunc* func;
        uint32_t           start;
        uint32_t           end;
        uint32_t           chunkIdx;
    };

    std::array<Task, MaxChunks>     tasks {};
    std::array<ChunkJob, MaxChunks> jobs {};

    // Zero-overhead address retrieval (Bypasses any custom operator& overloads)
    const DecayedFunc* funcPtr = std::addressof(func);

    for (uint32_t i = 0; i < numChunks; ++i) {
        uint32_t start = i * adjustedChunkSize;
        uint32_t end   = std::min(start + adjustedChunkSize, count);

        jobs[i] = {.func = funcPtr, .start = start, .end = end, .chunkIdx = i};

        tasks[i] = {
            .func =
                [](void* arg) {
                    auto* job = static_cast<ChunkJob*>(arg);
                    (*job->func)(job->start, job->end, job->chunkIdx);
                },
            .arg = &jobs[i]
        };
    }

    Counter sync;
    Dispatch(std::span<const Task>(tasks.data(), numChunks), &sync);
    Wait(&sync); // Fiber yields cleanly; stack frame stays 100% frozen and valid
}

} // namespace ZHLN::TaskSystem