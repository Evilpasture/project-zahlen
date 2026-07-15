// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TaskSystem.hpp"
#include "Thread.hpp"
#include "engine/Platform.hpp"
#include "threading/Mutex.hpp"
#include <condition_variable>
#include <mutex>
#include <queue> // Replaced vector with queue
#include <thread>

namespace ZHLN::TaskSystem {

// --- Thread-Safe Queue (Fixed: Now strictly FIFO) ---
struct WorkQueue {
    std::mutex              mtx;
    std::condition_variable cv;
    std::queue<Fiber*>      fibers;
    bool                    quit = false;

    void Push(Fiber* f) {
        std::lock_guard lock(mtx);
        fibers.push(f);
        cv.notify_one();
    }

    Fiber* PopOrWait() {
        std::unique_lock lock(mtx);
        cv.wait(lock, [this] { return !fibers.empty() || quit; });
        if (quit && fibers.empty()) {
            return nullptr;
        }
        Fiber* f = fibers.front(); // Pull from the front
        fibers.pop();              // Remove from the front
        return f;
    }

    Fiber* TryPop() {
        std::lock_guard lock(mtx);
        if (fibers.empty()) {
            return nullptr;
        }
        Fiber* f = fibers.front(); // Pull from the front
        fibers.pop();              // Remove from the front
        return f;
    }

    void WakeAll() {
        std::lock_guard lock(mtx);
        quit = true;
        cv.notify_all();
    }
};

// --- Thread-Local Cache Optimization ---
namespace {

// Compiler-safe single-element thread-local cache (maximum 1 fiber per thread)
static thread_local Fiber* t_localFiber = nullptr;

inline bool PushLocalFiber(Fiber* f) noexcept {
    if (t_localFiber == nullptr) {
        t_localFiber = f;
        return true;
    }
    return false; // Cache full, fallback to global s_freeQueue
}

inline Fiber* PopLocalFiber() noexcept {
    if (t_localFiber != nullptr) {
        Fiber* f     = t_localFiber;
        t_localFiber = nullptr;
        return f;
    }
    return nullptr; // Cache empty, fallback to global s_freeQueue
}

} // namespace

// --- Internal State ---
struct FiberData {
    Task     task;
    Counter* counter;
};

static WorkQueue                s_readyQueue;
static WorkQueue                s_freeQueue;
static std::vector<Fiber*>      s_fiberPool;
static std::vector<FiberData>   s_fiberData;
static std::vector<std::thread> s_threads;
static thread_local uint32_t    t_workerIndex = 0;
static uint32_t                 s_workerCount = 0;
struct TaskSystemDeinitGuard {
    ~TaskSystemDeinitGuard() {
        Shutdown();
    }
};
static TaskSystemDeinitGuard s_deinitGuard;

// --- The Infinite Loop every Fiber runs ---
static void FiberMain(void* arg) {
    auto* data = static_cast<FiberData*>(arg);
    while (true) {
        // 1. Run the assigned task
        if (data->task.func != nullptr) {
            data->task.func(data->task.arg);
        }

        // 2. Decrement counter if provided
        if (data->counter != nullptr) {
            data->counter->value.fetch_sub(1, std::memory_order::release);
        }

        // 3. Put this fiber back in the single-element local free pool
        Fiber* self = Fiber::GetCurrent();
        if (!PushLocalFiber(self)) {
            s_freeQueue.Push(self);
        }

        // 4. Yield back to the OS worker thread so it can grab the next Ready Fiber
        Fiber::Yield();
    }
}

// --- The Infinite Loop every OS Thread runs ---
static void WorkerMain(uint32_t index) {
    Platform::SetHighPriority();
    Fiber::InitMainThread();
    t_workerIndex = index;

    while (true) {
        Fiber* f = s_readyQueue.PopOrWait();
        if (f == nullptr) {
            break;
        }
        Fiber::Resume(f);
    }
}

void Init(uint32_t numThreads, uint32_t numFibers, size_t stackSize) {
    Platform::SetHighPriority();
    Fiber::InitMainThread();
    if (numThreads == 0) {
        numThreads = std::thread::hardware_concurrency();
        if (numThreads == 0) {
            numThreads = 4;
        }
        if (numThreads > 1) {
            numThreads -= 1; // Leave 1 core for the main loop
        }
    }

    s_workerCount = numThreads + 1;
    t_workerIndex = numThreads;

    s_fiberPool.resize(numFibers);
    s_fiberData.resize(numFibers);

    for (uint32_t i = 0; i < numFibers; i++) {
        s_fiberData[i] = {};
        s_fiberPool[i] = Fiber::Create(stackSize, FiberMain, &s_fiberData[i]);
        s_freeQueue.Push(s_fiberPool[i]);
    }

    for (uint32_t i = 0; i < numThreads; i++) {
        s_threads.emplace_back(WorkerMain, i);
    }
}

uint32_t GetWorkerIndex() {
    return t_workerIndex;
}
uint32_t GetWorkerCount() {
    return s_workerCount;
}

void Shutdown() {
    s_readyQueue.WakeAll();
    s_freeQueue.WakeAll();

    for (auto& t: s_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    s_threads.clear();

    for (Fiber* f: s_fiberPool) {
        Fiber::Destroy(f);
    }
    s_fiberPool.clear();
}

void Dispatch(std::span<const Task> tasks, Counter* counter) {
    if (tasks.empty()) {
        return;
    }

    if (counter != nullptr) {
        counter->value.fetch_add(tasks.size(), std::memory_order::relaxed);
    }

    for (const auto& task: tasks) {
        // Pull allocation-free from the single-element local cache first
        Fiber* f = PopLocalFiber();
        if (f == nullptr) {
            f = s_freeQueue.PopOrWait();
        }

        auto* data    = static_cast<FiberData*>(f->arg);
        data->task    = task;
        data->counter = counter;

        s_readyQueue.Push(f);
    }
}

void Wait(Counter* counter) {
    if (counter == nullptr) {
        return;
    }

    Fiber*   self      = Fiber::GetCurrent();
    uint32_t spinCount = 0;

    while (counter->value.load(std::memory_order::acquire) > 0) {
        bool isMain = (self == nullptr || self->isMain);

        if (isMain) {
            Fiber* f = s_readyQueue.TryPop();
            if (f != nullptr) {
                Fiber::Resume(f);
                spinCount = 0;
            } else {
                if (spinCount < 100) {
                    CPURelax();
                } else if (spinCount < 1000) {
                    for (int i = 0; i < 10; ++i) {
                        CPURelax();
                    }
                } else {
                    std::this_thread::yield();
                    spinCount = 0;
                }
                spinCount++;
            }
        } else {
            // Workers push themselves to the back of the line
            s_readyQueue.Push(self);
            Fiber::Yield();
        }
    }
}

void WakeUp(ZHLN::Fiber* fiber) {
    s_readyQueue.Push(fiber);
}

} // namespace ZHLN::TaskSystem
