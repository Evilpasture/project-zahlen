#pragma once
#include <cstdint>
#include <detail/Atomic.hpp>
#include <functional>
#include <span>
#include <vector>

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
	void* arg;
};

// A sync point to wait for a batch of jobs to finish
struct Counter {
	ZHLN::Atomic<uint32_t> value{0};
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

template <typename Func> void ParallelFor(uint32_t count, uint32_t chunkSize, Func&& func) {
	if (count == 0)
		return;
	if (count <= chunkSize) {
		func(0, count, 0); // start, end, chunkIdx
		return;
	}

	uint32_t numChunks = (count + chunkSize - 1) / chunkSize;
	std::vector<Task> tasks(numChunks);
	std::vector<std::function<void()>> jobs(numChunks);

	for (uint32_t i = 0; i < numChunks; ++i) {
		uint32_t start = i * chunkSize;
		uint32_t end = std::min(start + chunkSize, count);

		// Pass 'i' as the chunkIdx
		jobs[i] = [&func, start, end, i]() { func(start, end, i); };

		tasks[i].func = [](void* arg) {
			auto* job = static_cast<std::function<void()>*>(arg);
			(*job)();
		};
		tasks[i].arg = &jobs[i];
	}

	Counter sync;
	Dispatch(tasks, &sync);
	Wait(&sync);
}

} // namespace ZHLN::TaskSystem