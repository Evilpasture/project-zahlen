// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/FunctionRef.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace ZHLN::TaskSystem {
auto GetWorkerIndex() -> uint32_t;
auto GetWorkerCount() -> uint32_t;
using TaskFn = void (*)(void*);

struct Task {
    TaskFn func;
    void*  arg;
};

struct Counter {
    ZHLN::Atomic<uint32_t> value {0};
};

void Init(uint32_t numThreads = 0, uint32_t numFibers = 128, size_t stackSize = kMinimumFiberStackSize);

void Shutdown();

void Dispatch(std::span<const Task> tasks, Counter* counter = nullptr);

void Wait(Counter* counter);

void WakeUp(ZHLN::Fiber* fiber);

// Invoke callables in parallel and join all fibers before returning. The
// callables (including temporary arguments) remain alive for the whole call,
// so the Task ABI's void* trampoline and non-owning views stay internal.
template <typename... Funcs>
    requires(std::is_invocable_r_v<void, const std::remove_reference_t<Funcs>&> && ...)
void ParallelInvoke(Funcs&&... funcs) {
    constexpr size_t count = sizeof...(Funcs);
    if constexpr (count > 0) {
        std::array<FunctionRef<void() const>, count> borrowed {FunctionRef<void() const> {funcs}...};
        std::array<Task, count> tasks {};
        for (size_t i = 0; i < count; ++i) {
            tasks[i] = Task {
                .func = [](void* arg) { (*static_cast<FunctionRef<void() const>*>(arg))(); },
                .arg  = std::addressof(borrowed[i]),
            };
        }

        Counter sync;
        Dispatch(tasks, &sync);
        Wait(&sync);
    }
}

template <typename Func>
void ParallelFor(uint32_t count, uint32_t chunkSize, Func&& func) {
    if (count == 0) {
        return;
    }
    if (count <= chunkSize) {
        std::forward<Func>(func)(0, count, 0);
        return;
    }

    constexpr uint32_t MaxChunks         = 128;
    uint32_t           adjustedChunkSize = chunkSize;
    uint32_t           numChunks         = (count + adjustedChunkSize - 1) / adjustedChunkSize;

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

    const DecayedFunc* funcPtr = std::addressof(func);

    for (uint32_t i = 0; i < numChunks; ++i) {
        uint32_t start = i * adjustedChunkSize;
        uint32_t end   = std::min(start + adjustedChunkSize, count);

        jobs[i] = {.func = funcPtr, .start = start, .end = end, .chunkIdx = i};

        tasks[i] = {
            .func = [](void* arg) -> auto {
                auto* job = static_cast<ChunkJob*>(arg);
                (*job->func)(job->start, job->end, job->chunkIdx);
            },
            .arg = &jobs[i]
        };
    }

    Counter sync;
    Dispatch(std::span<const Task>(tasks.data(), numChunks), &sync);
    Wait(&sync);
}

}
