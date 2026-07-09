// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Thread.hpp"

#include "threading/Mutex.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <detail/Platform.hpp>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif
// Defined in Thread.S
extern "C" void ZHLN_Switch(void** old_sp, void* new_sp);
extern "C" void ZHLN_TrampolineAsm(void);

namespace ZHLN {

// Thread-local tracking of the active fiber
static thread_local Fiber t_mainFiber;
static thread_local Fiber* t_currentFiber = nullptr;

auto GetCurrentFiberID() -> uint64_t {
	if (t_currentFiber == nullptr) {
		return 0; // Not a fiber-managed thread
	}
	if (t_currentFiber->isMain) {
		return 1; // Friendly ID for main thread
	}

	// For worker fibers, return the memory address as a unique ID
	return std::bit_cast<uint64_t>(t_currentFiber);
}

/**
 * @brief The bridge between Assembly and C++.
 * This is the first code executed on a new fiber's stack.
 */
extern "C" void ZHLN_Trampoline() {
	Fiber* self = t_currentFiber;
	if (self->func != nullptr) {
		self->func(self->arg);
	}
	self->isFinished = true;

	// Fiber has returned. Yield back to the caller indefinitely.
	while (true) {
		Fiber::Yield();
	}
}

// Windows requires swapping StackBase/StackLimit in the TEB (Thread Environment Block)
static inline void SwapTEB(Fiber* target) {
#if defined(_WIN32)
	NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
	t_currentFiber->stackBase = tib->StackBase;
	t_currentFiber->stackLimit = tib->StackLimit;
	tib->StackBase = target->stackBase;
	tib->StackLimit = target->stackLimit;
#else
	(void)target;
#endif
}

Fiber* Fiber::GetCurrent() noexcept {
	return t_currentFiber;
}

void Fiber::InitMainThread() noexcept {
	if (t_currentFiber != nullptr) {
		return;
	}

	t_mainFiber.isFinished = false;
	t_mainFiber.isMain = true;
	t_mainFiber.caller = nullptr;

#if defined(_WIN32)
	NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
	t_mainFiber.stackBase = tib->StackBase;
	t_mainFiber.stackLimit = tib->StackLimit;
#endif

	t_currentFiber = &t_mainFiber;
}

Fiber* Fiber::Create(size_t stackSize, FiberFunc func, void* arg) noexcept {
	// 1. Calculate Sizes (Page Aligned)
#if defined(_WIN32)
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	size_t pageSize = si.dwPageSize;
#else
	size_t pageSize = sysconf(_SC_PAGESIZE);
#endif

	if (stackSize == 0) {
		stackSize = static_cast<size_t>(1024 * 1024); // 1MB default
	}
	stackSize = (stackSize + pageSize - 1) & ~(pageSize - 1);
	size_t totalSize = stackSize + (pageSize * 2); // Stack + 2 Guard Pages

	// 2. Allocate Stack
	void* map = nullptr;
#if defined(_WIN32)
	map = VirtualAlloc(nullptr, totalSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	DWORD old;
	VirtualProtect(map, pageSize, PAGE_READWRITE | PAGE_GUARD, &old);
#else
	map = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	mprotect(map, pageSize, PROT_NONE); // Bottom guard page
#endif

	// 3. Place Fiber struct at the very top of the allocated memory
	uintptr_t endAddr = std::bit_cast<uintptr_t>(map) + totalSize;
	uintptr_t structAddr = (endAddr - sizeof(Fiber)) & ~15ULL;
	auto* fiber = std::bit_cast<Fiber*>(structAddr);

	fiber->mapAddr = map;
	fiber->mapSize = totalSize;
	fiber->func = func;
	fiber->arg = arg;
	fiber->isFinished = false;
	fiber->isMain = false;

#if defined(_WIN32)
	fiber->stackBase = reinterpret_cast<void*>(endAddr);
	fiber->stackLimit = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(map) + pageSize);
#endif

	// 4. Initialize Stack Frame for mag_switch
	uintptr_t sp = structAddr;

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
	sp -= 240; // Windows x64 context size (GPRs + XMMs)
	*std::bit_cast<uintptr_t*>(sp + 232) = reinterpret_cast<uintptr_t>(ZHLN_TrampolineAsm);
#elif defined(__x86_64__) || defined(_M_X64)
	sp -= 8; // Space for Return Address
	*std::bit_cast<uintptr_t*>(sp) = reinterpret_cast<uintptr_t>(ZHLN_TrampolineAsm);
	sp -= 48; // Space for 6 Callee-saved GPRs
#elif defined(__aarch64__) || defined(_M_ARM64)
	sp -= 160; // ARM64 Context size
	*std::bit_cast<uintptr_t*>(sp + 88) =
		reinterpret_cast<uintptr_t>(ZHLN_TrampolineAsm); // X30 (LR)
#endif

	fiber->stackPointer = std::bit_cast<void*>(sp);
	return fiber;
}

void Fiber::Resume(Fiber* target) noexcept {
	//  Ensure the target OS thread has fully vacated this stack before we jump into it!
	while (target->isRunning.load(std::memory_order::acquire)) {
		CPURelax();
	}

	Fiber* self = t_currentFiber;
	target->caller = self;
	target->isRunning.store(true, std::memory_order::release);

	SwapTEB(target);
	t_currentFiber = target;

	// Execute Assembly Context Switch
	ZHLN_Switch(&self->stackPointer, target->stackPointer);

	// WE ARE BACK! The target has yielded to us, meaning it is safely off its stack!
	target->isRunning.store(false, std::memory_order::release);
}

void Fiber::Yield() noexcept {
	Fiber* self = t_currentFiber;
	Fiber* target = self->caller;
	if (target == nullptr) {
		return;
	}

	SwapTEB(target);
	t_currentFiber = target;
	ZHLN_Switch(&self->stackPointer, target->stackPointer);
}

void Fiber::Destroy(Fiber* fiber) noexcept {
	if ((fiber == nullptr) || fiber->isMain) {
		return;
	}
#if defined(_WIN32)
	VirtualFree(fiber->mapAddr, 0, MEM_RELEASE);
#else
	munmap(fiber->mapAddr, fiber->mapSize);
#endif
}

Fiber* GetCurrentFiber() noexcept {
	return Fiber::GetCurrent();
}

void YieldFiber() noexcept {
	Fiber::Yield();
}

} // namespace ZHLN
