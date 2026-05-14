#pragma once
#include <cstdint>
#include <detail/Atomic.hpp>
#include <span>

namespace ZHLN {
struct Fiber;
}

namespace ZHLN::TaskSystem {

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

} // namespace ZHLN::TaskSystem