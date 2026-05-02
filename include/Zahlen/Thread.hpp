#pragma once

#include <cstddef>

namespace ZHLN {

using FiberFunc = void (*)(void*);

/**
 * @brief High-performance Stackful Coroutine (Fiber).
 * Wraps the mag_asm backend in a C++ interface.
 */
struct alignas(128) Fiber {
	void* stackPointer; // Offset 0: Used by mag_switch
	void* mapAddr;		// Base of mmap/VirtualAlloc
	size_t mapSize;		// Total size including guard pages
	FiberFunc func;		// Entry point
	void* arg;			// User data
	Fiber* caller;		// Parent fiber to return to on Yield

#if defined(_WIN32)
	void* stackBase; // Windows TEB tracking
	void* stackLimit;
#endif

	bool isFinished;
	bool isMain;

	// Static API for Mutex/Scheduler access
	static Fiber* GetCurrent() noexcept;
	static void Yield() noexcept;
	static void Resume(Fiber* target) noexcept;

	// Allocation
	static Fiber* Create(size_t stackSize, FiberFunc func, void* arg) noexcept;
	static void Destroy(Fiber* fiber) noexcept;

	/**
	 * @brief Converts the current OS thread into the "Main" Fiber.
	 * Must be called once per thread before using fibers or Mutexes.
	 */
	static void InitMainThread() noexcept;
};

// Global Linker Satellites for Mutex.cpp
inline Fiber* GetCurrentFiber() noexcept {
	return Fiber::GetCurrent();
}
inline void YieldFiber() noexcept {
	Fiber::Yield();
}

} // namespace ZHLN