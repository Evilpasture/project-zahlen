// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/Platform.hpp"
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
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

    void PushSilent(Fiber* f) {
        std::lock_guard lock(mtx);
        fibers.push(f);
    }

    auto PopOrWait() -> Fiber* {
        std::unique_lock lock(mtx);
        cv.wait(lock, [this] -> bool { return !fibers.empty() || quit; });
        if (quit && fibers.empty()) {
            return nullptr;
        }
        Fiber* f = fibers.front(); // Pull from the front
        fibers.pop();              // Remove from the front
        return f;
    }

    auto TryPop() -> Fiber* {
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

    void Reset() {
        std::lock_guard    lock(mtx);
        std::queue<Fiber*> empty;
        fibers.swap(empty);
        quit = false;
    }
};

// --- Thread-Local Cache Optimization ---
namespace {

// Compiler-safe single-element thread-local cache (maximum 1 fiber per thread)
thread_local Fiber* t_localFiber = nullptr;

inline auto PushLocalFiber(Fiber* f) noexcept -> bool {
    if (t_localFiber == nullptr) {
        t_localFiber = f;
        return true;
    }
    return false; // Cache full, fallback to global s_freeQueue
}

inline auto PopLocalFiber() noexcept -> Fiber* {
    if (t_localFiber != nullptr) {
        Fiber* f     = t_localFiber;
        t_localFiber = nullptr;
        return f;
    }
    return nullptr; // Cache empty, fallback to global s_freeQueue
}

// --- Internal State ---
struct FiberData {
    Task     task;
    Counter* counter;
};

WorkQueue                s_readyQueue;
WorkQueue                s_freeQueue;
std::vector<Fiber*>      s_fiberPool;
std::vector<FiberData>   s_fiberData;
std::vector<std::thread> s_threads;
thread_local uint32_t    t_workerIndex = 0;
uint32_t                 s_workerCount = 0;
struct TaskSystemDeinitGuard {
    ~TaskSystemDeinitGuard() {
        Shutdown();
    }
};
TaskSystemDeinitGuard s_deinitGuard;

// --- The Infinite Loop every Fiber runs ---
void FiberMain(void* arg) {
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

        // 3. Mark the task complete. The RESUMER recycles this fiber into
        //    the free pool after Resume() returns -- never publish yourself
        //    here: between the push and the Yield the fiber is still running,
        //    and another thread that pops and resumes it becomes a second
        //    owner (both threads end up inside the same fiber; one of them
        //    returns from a Resume it never owned, and the fiber is dropped
        //    from every queue -- the pool silently drains until the root
        //    Dispatch starves).
        Fiber::GetCurrent()->taskDone.store(true, std::memory_order::release);

        // 4. Yield back to the OS worker thread so it can grab the next Ready Fiber
        Fiber::Yield();
    }
}

// Hand a fiber that just finished its task back to the pool. Called by the
// resumer immediately after Resume() returns: at that instant the fiber is
// provably suspended and this thread is its only owner. Blocked yields
// (mutex/condvar/counter waits) leave taskDone clear and are skipped --
// those fibers re-enter the ready queue through WakeUp instead.
inline void RecycleFiber(Fiber* f) noexcept {
    if (f == nullptr || !f->taskDone.exchange(false, std::memory_order::acquire)) {
        return;
    }
    if (!PushLocalFiber(f)) {
        s_freeQueue.Push(f);
    }
}

// --- The Infinite Loop every OS Thread runs ---
void WorkerMain(uint32_t index) {
    Platform::SetHighPriority();
    Fiber::InitMainThread();
    t_workerIndex = index;

    while (true) {
        // A fiber recycled into this thread's local cache is invisible to
        // every other thread. Hand it back to the global free pool BEFORE
        // sleeping: otherwise a root Dispatch can starve forever in
        // s_freeQueue.PopOrWait() while every free fiber sits parked in a
        // sleeping worker's cache (observed: whole pool idle at the post-task
        // Yield, both global queues empty, main hung dispatching entry nodes).
        if (Fiber* cached = PopLocalFiber()) {
            s_freeQueue.Push(cached);
        }
        Fiber* f = s_readyQueue.PopOrWait();
        if (f == nullptr) {
            break;
        }
        Fiber::Resume(f);
        RecycleFiber(f);
    }
}

} // namespace

void Init(uint32_t numThreads, uint32_t numFibers, size_t stackSize) {
    if (!s_threads.empty() || !s_fiberPool.empty()) {
        return;
    }
    Platform::SetHighPriority();
    Fiber::InitMainThread();
    s_readyQueue.Reset();
    s_freeQueue.Reset();
    t_localFiber = nullptr;
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

auto GetWorkerIndex() -> uint32_t {
    return t_workerIndex;
}
auto GetWorkerCount() -> uint32_t {
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
    s_fiberData.clear();
    s_readyQueue.Reset();
    s_freeQueue.Reset();
    t_localFiber  = nullptr;
    s_workerCount = 0;
}

void Dispatch(std::span<const Task> tasks, Counter* counter) {
    if (tasks.empty()) {
        return;
    }

    if (counter != nullptr) {
        counter->value.fetch_add(tasks.size(), std::memory_order::relaxed);
    }

    Fiber*     currentFiber   = Fiber::GetCurrent();
    const bool nestedDispatch = currentFiber != nullptr && !currentFiber->isMain;

    for (const auto& task: tasks) {
        // Parent task fibers must not block waiting for child fibers, but root
        // dispatch preserves asynchronous queue semantics. Running root jobs
        // inline can re-enter external job systems (notably Jolt) while they are
        // still constructing dependency graphs and invalidate queued jobs.
        Fiber* f = PopLocalFiber();
        if (f == nullptr) {
            f = nestedDispatch ? s_freeQueue.TryPop() : s_freeQueue.PopOrWait();
        }
        if (f == nullptr) {
            if (task.func != nullptr) {
                task.func(task.arg);
            }
            if (counter != nullptr) {
                counter->value.fetch_sub(1, std::memory_order::release);
            }
            continue;
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
                RecycleFiber(f);
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
            // Workers push themselves to the back of the line SILENTLY
            // to prevent condition variable thrashing and lost wakeups
            s_readyQueue.PushSilent(self);
            Fiber::Yield();
        }
    }
}

void WakeUp(ZHLN::Fiber* fiber) {
    s_readyQueue.Push(fiber);
}

} // namespace ZHLN::TaskSystem
