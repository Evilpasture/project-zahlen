// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TaskSystem.hpp"

#include "Thread.hpp"
#include "engine/Platform.hpp"
#include "threading/Mutex.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace ZHLN::TaskSystem {

// --- Thread-Safe Queue ---
struct WorkQueue {
	std::mutex mtx;
	std::condition_variable cv;
	std::vector<Fiber*> fibers;
	bool quit = false;

	void Push(Fiber* f) {
		std::lock_guard lock(mtx);
		fibers.push_back(f);
		cv.notify_one();
	}

	Fiber* PopOrWait() {
		std::unique_lock lock(mtx);
		cv.wait(lock, [this] { return !fibers.empty() || quit; });
		if (quit && fibers.empty()) {
			return nullptr;
		}
		Fiber* f = fibers.back();
		fibers.pop_back();
		return f;
	}

	Fiber* TryPop() {
		std::lock_guard lock(mtx);
		if (fibers.empty()) {
			return nullptr;
		}
		Fiber* f = fibers.back();
		fibers.pop_back();
		return f;
	}

	void WakeAll() {
		std::lock_guard lock(mtx);
		quit = true;
		cv.notify_all();
	}
};

// --- Internal State ---
struct FiberData {
	Task task;
	Counter* counter;
};

static WorkQueue s_readyQueue;
static WorkQueue s_freeQueue;
static std::vector<Fiber*> s_fiberPool;
static std::vector<FiberData> s_fiberData;
static std::vector<std::thread> s_threads;
static thread_local uint32_t t_workerIndex = 0;
static uint32_t s_workerCount = 0;
struct TaskSystemDeinitGuard {
	~TaskSystemDeinitGuard() { Shutdown(); }
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

		// 3. We are done! Put ourselves back in the free pool
		s_freeQueue.Push(Fiber::GetCurrent());

		// 4. Yield back to the OS worker thread so it can grab the next Ready Fiber
		Fiber::Yield();

		// When we wake up here later, it's because Dispatch() gave us a new task!
	}
}

// --- The Infinite Loop every OS Thread runs ---
static void WorkerMain(uint32_t index) {
	Platform::SetHighPriority();
	Fiber::InitMainThread();
	t_workerIndex = index; // Store it in thread_local

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
	// The thread that calls Init() (the main thread) must be a Fiber to use Wait()
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
		// The 'arg' we pass is a pointer to this fiber's specific data slot
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

	for (auto& t : s_threads) {
		if (t.joinable()) {
			t.join();
		}
	}
	s_threads.clear();

	for (Fiber* f : s_fiberPool) {
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

	for (const auto& task : tasks) {
		// Grab a sleeping fiber
		Fiber* f = s_freeQueue.PopOrWait();
		if (f == nullptr) {
			return;
		}

		// Assign the task to its data payload
		auto* data = static_cast<FiberData*>(f->arg);
		data->task = task;
		data->counter = counter;

		// Put it in the queue for the OS Threads to pick up
		s_readyQueue.Push(f);
	}
}

void Wait(Counter* counter) {
	if (counter == nullptr) {
		return;
	}

	Fiber* self = Fiber::GetCurrent();
	uint32_t spinCount = 0;

	while (counter->value.load(std::memory_order::acquire) > 0) {
		bool isMain = (self == nullptr || self->isMain);

		if (isMain) {
			Fiber* f = s_readyQueue.TryPop();
			if (f != nullptr) {
				Fiber::Resume(f);
				spinCount = 0;
			} else {
				// On macOS, we DO NOT YIELD here.
				// We spin to keep the P-core "Hot" so the OS doesn't
				// context switch us for a trackpad interrupt.
				if (spinCount < 100) {
					CPURelax();
				} else if (spinCount < 1000) {
					// Small backoff but still no OS yield
					for (int i = 0; i < 10; ++i) {
						CPURelax();
					}
				} else {
					// Only after 1000 failures do we finally give the OS
					// a tiny bit of breathing room.
					std::this_thread::yield();
					spinCount = 0; // Reset to start hot again
				}
				spinCount++;
			}
		} else {
			// Workers should just push themselves back and yield to the scheduler
			s_readyQueue.Push(self);
			Fiber::Yield();
		}
	}
}

void WakeUp(ZHLN::Fiber* fiber) {
	s_readyQueue.Push(fiber);
}

} // namespace ZHLN::TaskSystem
