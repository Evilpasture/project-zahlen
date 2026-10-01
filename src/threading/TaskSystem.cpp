// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace ZHLN::TaskSystem {

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
        Fiber* f = fibers.front();
        fibers.pop();
        return f;
    }

    auto TryPop() -> Fiber* {
        std::lock_guard lock(mtx);
        if (fibers.empty()) {
            return nullptr;
        }
        Fiber* f = fibers.front();
        fibers.pop();
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

namespace {

thread_local Fiber* t_localFiber = nullptr;

inline auto PushLocalFiber(Fiber* f) noexcept -> bool {
    if (t_localFiber == nullptr) {
        t_localFiber = f;
        return true;
    }
    return false;
}

inline auto PopLocalFiber() noexcept -> Fiber* {
    if (t_localFiber != nullptr) {
        Fiber* f     = t_localFiber;
        t_localFiber = nullptr;
        return f;
    }
    return nullptr;
}

struct FiberData {
    Task     task;
    Counter* counter;
};

WorkQueue                s_readyQueue;
WorkQueue                s_freeQueue;
std::vector<Fiber*>      s_fiberPool;
std::vector<FiberData>   s_fiberData;
std::vector<std::thread> s_threads;

// Lifecycle transitions (including worker joins) are serialized. WorkerMain's
// queue loop does not take this lock; all submitted tasks must finish before
// Shutdown joins the workers.
std::mutex               s_lifecycleMutex;
bool                     s_running = false; // guarded by s_lifecycleMutex
std::atomic<uint32_t>    s_workerCount {0};
thread_local uint32_t    t_workerIndex = 0;

void SetCurrentThreadHighPriority() noexcept {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

void FiberMain(void* arg) {
    auto* data = static_cast<FiberData*>(arg);
    while (true) {
        if (data->task.func != nullptr) {
            data->task.func(data->task.arg);
        }

        if (data->counter != nullptr) {
            data->counter->value.fetch_sub(1, std::memory_order::release);
        }

        Fiber::GetCurrent()->taskDone.store(true, std::memory_order::release);

        Fiber::Yield();
    }
}

inline void RecycleFiber(Fiber* f) noexcept {
    if (f == nullptr || !f->taskDone.exchange(false, std::memory_order::acquire)) {
        return;
    }
    if (!PushLocalFiber(f)) {
        s_freeQueue.Push(f);
    }
}

void WorkerMain(uint32_t index) {
    SetCurrentThreadHighPriority();
    Fiber::InitMainThread();
    t_workerIndex = index;

    while (true) {
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

}

void Init(uint32_t numThreads, uint32_t numFibers, size_t stackSize) {
    std::lock_guard lock(s_lifecycleMutex);
    if (s_running) {
        return;
    }
    SetCurrentThreadHighPriority();
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
            numThreads -= 1;
        }
    }

    s_workerCount.store(numThreads + 1, std::memory_order::release);
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
    s_running = true;
}

auto GetWorkerIndex() -> uint32_t {
    return t_workerIndex;
}
auto GetWorkerCount() -> uint32_t {
    return s_workerCount.load(std::memory_order::acquire);
}

void Shutdown() {
    std::lock_guard lock(s_lifecycleMutex);
    if (!s_running) {
        return;
    }

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
    t_localFiber = nullptr;
    s_workerCount.store(0, std::memory_order::release);
    s_running = false;
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
            s_readyQueue.PushSilent(self);
            Fiber::Yield();
        }
    }
}

void WakeUp(ZHLN::Fiber* fiber) {
    s_readyQueue.Push(fiber);
}

}
