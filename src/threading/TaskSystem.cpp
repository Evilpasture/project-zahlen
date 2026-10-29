// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace ZHLN::TaskSystem {

namespace {

class BoundedFiberQueue {
  public:
    void Reset(size_t minCapacity) {
        const size_t capacity = std::bit_ceil(std::max<size_t>(minCapacity, 64));
        _mask                 = capacity - 1;
        _cells                = std::make_unique<Cell[]>(capacity);
        for (size_t i = 0; i < capacity; ++i) {
            _cells[i].sequence.store(i, std::memory_order::relaxed);
            _cells[i].fiber = nullptr;
        }
        _enqueuePos.store(0, std::memory_order::relaxed);
        _dequeuePos.store(0, std::memory_order::relaxed);
    }

    auto TryPush(Fiber* fiber) noexcept -> bool {
        Cell*  cell = nullptr;
        size_t pos  = _enqueuePos.load(std::memory_order::relaxed);
        for (;;) {
            cell             = &_cells[pos & _mask];
            const size_t seq = cell->sequence.load(std::memory_order::acquire);
            const auto   dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (dif == 0) {
                if (_enqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order::relaxed, std::memory_order::relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false;
            } else {
                pos = _enqueuePos.load(std::memory_order::relaxed);
            }
        }
        cell->fiber = fiber;
        cell->sequence.store(pos + 1, std::memory_order::release);
        return true;
    }

    [[nodiscard]] auto TryPop() noexcept -> Fiber* {
        Cell*  cell = nullptr;
        size_t pos  = _dequeuePos.load(std::memory_order::relaxed);
        for (;;) {
            cell             = &_cells[pos & _mask];
            const size_t seq = cell->sequence.load(std::memory_order::acquire);
            const auto   dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (dif == 0) {
                if (_dequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order::relaxed, std::memory_order::relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return nullptr;
            } else {
                pos = _dequeuePos.load(std::memory_order::relaxed);
            }
        }
        Fiber* fiber = cell->fiber;
        cell->fiber  = nullptr;
        cell->sequence.store(pos + _mask + 1, std::memory_order::release);
        return fiber;
    }

    [[nodiscard]] auto Empty() const noexcept -> bool {
        if (!_cells) {
            return true;
        }
        const size_t pos = _dequeuePos.load(std::memory_order::relaxed);
        const size_t seq = _cells[pos & _mask].sequence.load(std::memory_order::acquire);
        return static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1) < 0;
    }

  private:
    struct Cell {
        ZHLN::Atomic<size_t> sequence {0};
        Fiber*               fiber = nullptr;
    };

    std::unique_ptr<Cell[]>          _cells;
    size_t                           _mask = 0;
    alignas(64) ZHLN::Atomic<size_t> _enqueuePos {0};
    alignas(64) ZHLN::Atomic<size_t> _dequeuePos {0};
};

struct alignas(128) WorkerSlot {
    BoundedFiberQueue readyQueue;
    // Separate queue: main-thread Dispatch/Wait work stealing never runs these jobs.
    BoundedFiberQueue backgroundQueue;
    BoundedFiberQueue freeQueue;
};

struct FiberData {
    Task     task {};
    Counter* counter = nullptr;
};

std::unique_ptr<WorkerSlot[]> s_workerSlots;
std::vector<Fiber*>           s_fiberPool;
std::vector<FiberData>        s_fiberData;
std::vector<std::thread>      s_threads;

// Lifecycle transitions (including worker joins) are serialized. WorkerMain's
// hot queue loop is lock-free and only enters s_sleepMutex when parking.
std::mutex                            s_lifecycleMutex;
bool                                  s_running = false;
alignas(64) ZHLN::Atomic<uint32_t>    s_workerCount {0};
alignas(64) ZHLN::Atomic<uint32_t>    s_parkedWorkers {0};
alignas(64) ZHLN::Atomic<bool>        s_stopRequested {false};
alignas(64) ZHLN::Atomic<uint32_t>    s_dispatchCursor {0};
std::mutex                            s_sleepMutex;
std::condition_variable               s_sleepCv;

thread_local uint32_t t_workerIndex = 0;

void SetCurrentThreadHighPriority() noexcept {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

[[nodiscard]] auto ClampSlot(uint32_t slot, uint32_t totalWorkers) noexcept -> uint32_t {
    return slot < totalWorkers ? slot : 0;
}

[[nodiscard]] auto PopOrStealReady(uint32_t preferredSlot, uint32_t totalWorkers) noexcept -> Fiber* {
    if (totalWorkers == 0 || !s_workerSlots) {
        return nullptr;
    }
    const uint32_t base = ClampSlot(preferredSlot, totalWorkers);
    if (Fiber* local = s_workerSlots[base].readyQueue.TryPop()) {
        return local;
    }
    for (uint32_t offset = 1; offset < totalWorkers; ++offset) {
        const uint32_t victim = (base + offset) % totalWorkers;
        if (Fiber* stolen = s_workerSlots[victim].readyQueue.TryPop()) {
            return stolen;
        }
    }
    return nullptr;
}

[[nodiscard]] auto PopOrStealBackground(uint32_t preferredSlot, uint32_t totalWorkers) noexcept -> Fiber* {
    if (totalWorkers <= 1 || !s_workerSlots) {
        return nullptr;
    }
    const uint32_t backgroundWorkers = totalWorkers - 1;
    const uint32_t base = ClampSlot(preferredSlot, backgroundWorkers);
    if (Fiber* local = s_workerSlots[base].backgroundQueue.TryPop()) {
        return local;
    }
    for (uint32_t offset = 1; offset < backgroundWorkers; ++offset) {
        const uint32_t victim = (base + offset) % backgroundWorkers;
        if (Fiber* stolen = s_workerSlots[victim].backgroundQueue.TryPop()) {
            return stolen;
        }
    }
    return nullptr;
}

[[nodiscard]] auto PopOrStealWorkerReady(uint32_t preferredSlot, uint32_t totalWorkers) noexcept -> Fiber* {
    if (Fiber* background = PopOrStealBackground(preferredSlot, totalWorkers)) {
        return background;
    }
    return PopOrStealReady(preferredSlot, totalWorkers);
}

[[nodiscard]] auto HasAnyReadyWork(uint32_t totalWorkers) noexcept -> bool {
    if (totalWorkers == 0 || !s_workerSlots) {
        return false;
    }
    for (uint32_t i = 0; i < totalWorkers; ++i) {
        if (!s_workerSlots[i].readyQueue.Empty() || !s_workerSlots[i].backgroundQueue.Empty()) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto AllocateFiber(uint32_t preferredSlot, uint32_t totalWorkers) noexcept -> Fiber* {
    if (totalWorkers == 0 || !s_workerSlots) {
        return nullptr;
    }
    const uint32_t base = ClampSlot(preferredSlot, totalWorkers);
    if (Fiber* local = s_workerSlots[base].freeQueue.TryPop()) {
        return local;
    }
    for (uint32_t offset = 1; offset < totalWorkers; ++offset) {
        const uint32_t donor = (base + offset) % totalWorkers;
        if (Fiber* stolen = s_workerSlots[donor].freeQueue.TryPop()) {
            return stolen;
        }
    }
    return nullptr;
}

void RecycleFiber(Fiber* fiber, uint32_t preferredSlot, uint32_t totalWorkers) noexcept {
    if (fiber == nullptr || !fiber->taskDone.exchange(false, std::memory_order::acquire)) {
        return;
    }
    if (totalWorkers > 0 && s_workerSlots) {
        s_workerSlots[ClampSlot(preferredSlot, totalWorkers)].freeQueue.TryPush(fiber);
    }
}

void NotifyParkedWorkers(size_t pushedCount) noexcept {
    if (pushedCount == 0) {
        return;
    }
    std::atomic_thread_fence(std::memory_order::seq_cst);
    if (s_parkedWorkers.load(std::memory_order::relaxed) == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_sleepMutex);
    }
    if (pushedCount == 1) {
        s_sleepCv.notify_one();
    } else {
        s_sleepCv.notify_all();
    }
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

void WorkerMain(uint32_t index, uint32_t totalWorkers) {
    SetCurrentThreadHighPriority();
    Fiber::InitMainThread();
    t_workerIndex = index;

    constexpr uint32_t kSpinIterations = 32;

    for (;;) {
        if (Fiber* fiber = PopOrStealWorkerReady(index, totalWorkers)) {
            Fiber::Resume(fiber);
            RecycleFiber(fiber, index, totalWorkers);
            continue;
        }

        Fiber* spunFiber = nullptr;
        for (uint32_t spin = 0; spin < kSpinIterations; ++spin) {
            if (s_stopRequested.load(std::memory_order::acquire)) {
                break;
            }
            CPURelax();
            spunFiber = PopOrStealWorkerReady(index, totalWorkers);
            if (spunFiber != nullptr) {
                break;
            }
        }
        if (spunFiber != nullptr) {
            Fiber::Resume(spunFiber);
            RecycleFiber(spunFiber, index, totalWorkers);
            continue;
        }

        s_parkedWorkers.fetch_add(1, std::memory_order::seq_cst);

        if (Fiber* lateFiber = PopOrStealWorkerReady(index, totalWorkers)) {
            s_parkedWorkers.fetch_sub(1, std::memory_order::relaxed);
            Fiber::Resume(lateFiber);
            RecycleFiber(lateFiber, index, totalWorkers);
            continue;
        }

        if (s_stopRequested.load(std::memory_order::acquire)) {
            s_parkedWorkers.fetch_sub(1, std::memory_order::relaxed);
            break;
        }

        {
            std::unique_lock<std::mutex> lock(s_sleepMutex);
            s_sleepCv.wait(lock, [totalWorkers] -> bool {
                return s_stopRequested.load(std::memory_order::acquire) || HasAnyReadyWork(totalWorkers);
            });
        }

        s_parkedWorkers.fetch_sub(1, std::memory_order::relaxed);

        if (s_stopRequested.load(std::memory_order::acquire) && !HasAnyReadyWork(totalWorkers)) {
            break;
        }
    }
}

} // namespace

void Init(uint32_t numThreads, uint32_t numFibers, size_t stackSize) {
    std::lock_guard<std::mutex> lock(s_lifecycleMutex);
    if (s_running) {
        return;
    }

    SetCurrentThreadHighPriority();
    Fiber::InitMainThread();

    if (numThreads == 0) {
        numThreads = std::thread::hardware_concurrency();
        if (numThreads == 0) {
            numThreads = 4;
        }
        if (numThreads > 1) {
            numThreads -= 1;
        }
    }

    const uint32_t totalWorkers = numThreads + 1;
    const size_t   ringCapacity = static_cast<size_t>(numFibers) * 2 + 16;

    s_stopRequested.store(false, std::memory_order::relaxed);
    s_parkedWorkers.store(0, std::memory_order::relaxed);
    s_dispatchCursor.store(0, std::memory_order::relaxed);

    s_workerSlots = std::make_unique<WorkerSlot[]>(totalWorkers);
    for (uint32_t slot = 0; slot < totalWorkers; ++slot) {
        s_workerSlots[slot].readyQueue.Reset(ringCapacity);
        s_workerSlots[slot].backgroundQueue.Reset(ringCapacity);
        s_workerSlots[slot].freeQueue.Reset(ringCapacity);
    }

    t_workerIndex = numThreads;

    s_fiberPool.resize(numFibers);
    s_fiberData.resize(numFibers);

    for (uint32_t i = 0; i < numFibers; ++i) {
        s_fiberData[i] = {};
        s_fiberPool[i] = Fiber::Create(stackSize, FiberMain, &s_fiberData[i]);
        s_workerSlots[i % totalWorkers].freeQueue.TryPush(s_fiberPool[i]);
    }

    s_workerCount.store(totalWorkers, std::memory_order::release);
    s_running = true;

    s_threads.reserve(numThreads);
    for (uint32_t i = 0; i < numThreads; ++i) {
        s_threads.emplace_back(WorkerMain, i, totalWorkers);
    }
}

auto GetWorkerIndex() -> uint32_t {
    return t_workerIndex;
}

auto GetWorkerCount() -> uint32_t {
    return s_workerCount.load(std::memory_order::acquire);
}

auto DispatchBackground(const Task task, Counter* counter) noexcept
    -> std::expected<void, BackgroundDispatchError> {
    if (task.func == nullptr) {
        return std::unexpected(BackgroundDispatchError::InvalidTask);
    }

    const uint32_t totalWorkers = s_workerCount.load(std::memory_order::acquire);
    if (totalWorkers == 0 || !s_workerSlots) {
        return std::unexpected(BackgroundDispatchError::SchedulerUnavailable);
    }
    if (totalWorkers <= 1) {
        return std::unexpected(BackgroundDispatchError::NoBackgroundWorkers);
    }

    const uint32_t backgroundWorkers = totalWorkers - 1;
    const uint32_t targetSlot = s_dispatchCursor.fetch_add(1, std::memory_order::relaxed) % backgroundWorkers;
    Fiber* fiber = AllocateFiber(targetSlot, totalWorkers);
    if (fiber == nullptr) {
        return std::unexpected(BackgroundDispatchError::NoAvailableFiber);
    }

    auto* data = static_cast<FiberData*>(fiber->arg);
    data->task = task;
    data->counter = counter;
    if (counter != nullptr) {
        counter->value.fetch_add(1, std::memory_order::relaxed);
    }

    if (!s_workerSlots[targetSlot].backgroundQueue.TryPush(fiber)) {
        if (counter != nullptr) {
            counter->value.fetch_sub(1, std::memory_order::release);
        }
        data->task = {};
        data->counter = nullptr;
        s_workerSlots[targetSlot].freeQueue.TryPush(fiber);
        return std::unexpected(BackgroundDispatchError::QueueFull);
    }

    NotifyParkedWorkers(1);
    return {};
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(s_lifecycleMutex);
    if (!s_running) {
        return;
    }

    s_stopRequested.store(true, std::memory_order::seq_cst);
    {
        std::lock_guard<std::mutex> sleepLock(s_sleepMutex);
    }
    s_sleepCv.notify_all();

    for (auto& thread: s_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    s_threads.clear();

    s_workerCount.store(0, std::memory_order::release);
    s_running = false;

    for (Fiber* fiber: s_fiberPool) {
        Fiber::Destroy(fiber);
    }
    s_fiberPool.clear();
    s_fiberData.clear();
    s_workerSlots.reset();

    s_parkedWorkers.store(0, std::memory_order::relaxed);
    s_stopRequested.store(false, std::memory_order::relaxed);
}

void Dispatch(std::span<const Task> tasks, Counter* counter) {
    if (tasks.empty()) {
        return;
    }

    const uint32_t totalWorkers = s_workerCount.load(std::memory_order::acquire);
    if (totalWorkers == 0) {
        for (const auto& task: tasks) {
            if (task.func != nullptr) {
                task.func(task.arg);
            }
        }
        return;
    }

    if (counter != nullptr) {
        counter->value.fetch_add(static_cast<uint32_t>(tasks.size()), std::memory_order::relaxed);
    }

    Fiber*         currentFiber = Fiber::GetCurrent();
    const bool     isMain       = (currentFiber == nullptr || currentFiber->isMain);
    const uint32_t localSlot    = ClampSlot(t_workerIndex, totalWorkers);
    const uint32_t startTarget  = tasks.size() == 1
                                      ? localSlot
                                      : (s_dispatchCursor.fetch_add(static_cast<uint32_t>(tasks.size()), std::memory_order::relaxed) % totalWorkers);

    size_t pushedCount = 0;

    for (size_t i = 0; i < tasks.size(); ++i) {
        const Task& task  = tasks[i];
        Fiber*      fiber = AllocateFiber(localSlot, totalWorkers);

        if (fiber == nullptr && isMain) {
            if (pushedCount > 0) {
                NotifyParkedWorkers(pushedCount);
                pushedCount = 0;
            }
            if (Fiber* ready = PopOrStealReady(localSlot, totalWorkers)) {
                Fiber::Resume(ready);
                RecycleFiber(ready, localSlot, totalWorkers);
                fiber = AllocateFiber(localSlot, totalWorkers);
            }
        }

        if (fiber == nullptr) {
            if (pushedCount > 0) {
                NotifyParkedWorkers(pushedCount);
                pushedCount = 0;
            }
            if (task.func != nullptr) {
                task.func(task.arg);
            }
            if (counter != nullptr) {
                counter->value.fetch_sub(1, std::memory_order::release);
            }
            continue;
        }

        auto* data    = static_cast<FiberData*>(fiber->arg);
        data->task    = task;
        data->counter = counter;

        const uint32_t targetSlot = static_cast<uint32_t>((startTarget + i) % totalWorkers);
        s_workerSlots[targetSlot].readyQueue.TryPush(fiber);
        ++pushedCount;
    }

    if (pushedCount > 0) {
        NotifyParkedWorkers(pushedCount);
    }
}

void Wait(Counter* counter) {
    if (counter == nullptr) {
        return;
    }

    Fiber*         self      = Fiber::GetCurrent();
    const bool     isMain    = (self == nullptr || self->isMain);
    uint32_t       spinCount = 0;

    while (counter->value.load(std::memory_order::acquire) > 0) {
        const uint32_t totalWorkers = s_workerCount.load(std::memory_order::acquire);
        if (totalWorkers == 0) {
            CPURelax();
            continue;
        }
        const uint32_t localSlot = ClampSlot(t_workerIndex, totalWorkers);

        if (isMain) {
            if (Fiber* fiber = PopOrStealReady(localSlot, totalWorkers)) {
                Fiber::Resume(fiber);
                RecycleFiber(fiber, localSlot, totalWorkers);
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
                ++spinCount;
            }
        } else {
            s_workerSlots[localSlot].readyQueue.TryPush(self);
            NotifyParkedWorkers(1);
            Fiber::Yield();
        }
    }
}

void WakeUp(ZHLN::Fiber* fiber) {
    if (fiber == nullptr) {
        return;
    }
    const uint32_t totalWorkers = s_workerCount.load(std::memory_order::acquire);
    if (totalWorkers == 0 || !s_workerSlots) {
        return;
    }
    const uint32_t localSlot = ClampSlot(t_workerIndex, totalWorkers);
    s_workerSlots[localSlot].readyQueue.TryPush(fiber);
    NotifyParkedWorkers(1);
}

} // namespace ZHLN::TaskSystem
